// Reactive variables and negated aliases, lowered after parsing into ordinary Flywheel code.
//
//   reactive bool alive = true     a variable with a hidden watcher list, __fwr_w_alive
//   alive = false                  the assignment, then every watcher of `alive` runs
//   computed bool dead = not alive a variable that a watcher on `alive` recomputes (eagerly)
//   when dead do ... end           a closure, registered with each reactive or computed
//                                  variable its condition reads, that runs the body when the
//                                  condition becomes true (and re-arms when it becomes false)
//   when c do ... else do ... end  the else body runs each time c becomes false after being true
//   when p.health <= 0 do          a field the condition reads has a watcher list of its own,
//                                  __fwr_field_health, that every `x.health = ...` notifies
//   when touching(goal) do         the condition also follows the top-level variables and
//                                  fields read by the functions and methods it calls
//   defer when c do ... end        changes only mark the check pending; it runs after each
//                                  statement of the code that follows it, not mid-call
//   bool enabled/disabled = true   `disabled` reads `not enabled`; `disabled = x` assigns
//                                  `enabled = not x`
//   int n = 0; when n > 3 do       a plain variable that a `when` or `computed` reads becomes
//                                  reactive, as if declared `reactive int n = 0`
//
// Fields are matched by name, whatever the object: `a.health = 1` notifies a condition that reads
// `b.health`, and the check finds the condition unchanged. Methods are matched by name too.
//
// Names resolve lexically (functions see top-level declarations; locals, parameters, loop
// variables, and catch bindings shadow them), so the lowering agrees with the compiler's scopes.
// A `when` or `computed` inside a block unregisters its watchers when the block ends; at the top
// level they stay registered for the whole program.
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fw {
namespace {
struct Binding {
    enum Kind { Plain, Reactive, Computed, Alias } kind = Plain;
    std::string target; // Alias: the bool variable it negates
};
EP make(const std::string &kind, const std::string &text, Location loc) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->text = text;
    e->loc = loc;
    return e;
}
void relocate(Expr &e, Location loc);
void relocate(std::vector<SP> &body, Location loc) {
    for (auto &s : body) {
        s->loc = loc;
        if (s->expr)
            relocate(*s->expr, loc);
        if (s->target)
            relocate(*s->target, loc);
        relocate(s->body, loc);
        relocate(s->other, loc);
        relocate(s->finalizer, loc);
        for (auto &c : s->catches)
            relocate(c, loc);
    }
}
void relocate(Expr &e, Location loc) {
    e.loc = loc;
    for (auto &a : e.args)
        if (a)
            relocate(*a, loc);
    if (e.lambda) {
        e.lambda->loc = loc;
        relocate(e.lambda->body, loc);
    }
}
// Parses generated Flywheel statements, placed at `loc` for diagnostics.
std::vector<SP> generated(const std::string &text, Location loc) {
    auto program = Parser(lex(text)).parse();
    relocate(program.body, loc);
    return std::move(program.body);
}
const char *watcherType = "array<function() returns void>";

// What an expression or a function body reads: plain names, fields (member accesses and
// `this.field` reads that are not called), and the functions and methods it calls.
struct Reads {
    std::vector<std::string> names, fields;
    std::vector<Function *> calls;
};
void addOnce(std::vector<std::string> &list, const std::string &item) {
    if (std::find(list.begin(), list.end(), item) == list.end())
        list.push_back(item);
}

class Lowerer {
    std::vector<std::map<std::string, Binding>> scopes;
    int counter = 0;
    // Names that a `when` condition or `computed` expression reads. A plain variable with one of
    // these names is made reactive, so `when delay == 10s do` works on `int[time] delay`.
    std::set<std::string> watched;
    // Top-level variables read by the functions a condition calls: made reactive at the top level.
    std::set<std::string> watchedTop;
    // Fields that conditions read, directly or through the functions they call.
    std::set<std::string> fields;
    std::map<std::string, std::vector<Function *>> functionsByName, methodsByName;
    std::map<Function *, Reads> direct; // what each function's own body reads

    void scan(Expr &e, Reads &r, const std::set<std::string> *locals) {
        auto local = [&](const std::string &n) { return locals && locals->count(n); };
        if (e.kind == "name") {
            if (!local(e.text))
                addOnce(r.names, e.text);
            return;
        }
        if (e.kind == "inst") { // this.field
            addOnce(r.fields, e.text);
            return;
        }
        if (e.kind == "member" && !e.args.empty() && e.args[0]) {
            addOnce(r.fields, e.text);
            scan(*e.args[0], r, locals);
            return;
        }
        size_t from = 0;
        if (e.kind == "call" && !e.args.empty() && e.args[0]) {
            auto &callee = *e.args[0];
            std::vector<Function *> *found = nullptr;
            if (callee.kind == "name" && !local(callee.text)) {
                if (auto it = functionsByName.find(callee.text); it != functionsByName.end())
                    found = &it->second;
                else
                    addOnce(r.names, callee.text); // a function-valued variable
            } else if ((callee.kind == "member" && !callee.args.empty() && callee.args[0]) ||
                       callee.kind == "inst") {
                if (auto it = methodsByName.find(callee.text); it != methodsByName.end())
                    found = &it->second;
                if (callee.kind == "member")
                    scan(*callee.args[0], r, locals);
            } else
                scan(callee, r, locals);
            if (found)
                for (auto *f : *found)
                    if (std::find(r.calls.begin(), r.calls.end(), f) == r.calls.end())
                        r.calls.push_back(f);
            from = 1;
        }
        for (size_t i = from; i < e.args.size(); ++i)
            if (e.args[i])
                scan(*e.args[i], r, locals);
        if (e.lambda)
            scanBody(e.lambda->body, r, locals);
    }
    void scanBody(std::vector<SP> &body, Reads &r, const std::set<std::string> *locals) {
        for (auto &s : body) {
            if (s->expr)
                scan(*s->expr, r, locals);
            if (s->target) {
                // An assignment writes its target: only the object and index parts are read.
                if (s->kind == "assign" && s->target->kind != "name") {
                    for (auto &a : s->target->args)
                        if (a)
                            scan(*a, r, locals);
                } else if (s->kind != "assign")
                    scan(*s->target, r, locals);
            }
            for (auto &g : s->guards)
                if (g)
                    scan(*g, r, locals);
            scanBody(s->body, r, locals);
            scanBody(s->other, r, locals);
            scanBody(s->finalizer, r, locals);
            for (auto &c : s->catches)
                scanBody(c, r, locals);
        }
    }
    static void declared(std::vector<SP> &body, std::set<std::string> &out) {
        std::function<void(Expr &)> inExpr = [&](Expr &e) {
            for (auto &a : e.args)
                if (a)
                    inExpr(*a);
            if (e.lambda) {
                for (auto &p : e.lambda->params)
                    out.insert(p.name);
                declared(e.lambda->body, out);
            }
        };
        std::function<void(Expr &)> patternNames = [&](Expr &p) {
            if (p.kind == "name")
                out.insert(p.text);
            for (auto &a : p.args)
                if (a)
                    patternNames(*a);
        };
        for (auto &s : body) {
            if ((s->kind == "declare" || s->kind == "for" || s->kind == "destructure") &&
                !s->name.empty())
                out.insert(s->name);
            if (s->kind == "for" && !s->op.empty())
                out.insert(s->op);
            if (!s->alias.empty())
                out.insert(s->alias);
            for (auto &n : s->catchNames)
                if (!n.empty())
                    out.insert(n);
            for (auto &ps : s->patterns)
                for (auto &p : ps)
                    if (p)
                        patternNames(*p);
            if (s->expr)
                inExpr(*s->expr);
            if (s->target)
                inExpr(*s->target);
            declared(s->body, out);
            declared(s->other, out);
            declared(s->finalizer, out);
            for (auto &c : s->catches)
                declared(c, out);
        }
    }
    Reads &own(Function *f) {
        if (auto it = direct.find(f); it != direct.end())
            return it->second;
        std::set<std::string> locals;
        for (auto &p : f->params)
            locals.insert(p.name);
        declared(f->body, locals);
        Reads r;
        scanBody(f->body, r, &locals);
        return direct[f] = std::move(r);
    }
    // Everything the called functions read, following their calls in turn.
    Reads called(const std::vector<Function *> &calls) {
        Reads out;
        std::set<Function *> seen;
        std::vector<Function *> work(calls.begin(), calls.end());
        while (!work.empty()) {
            auto *f = work.back();
            work.pop_back();
            if (!seen.insert(f).second)
                continue;
            auto &r = own(f);
            for (auto &n : r.names)
                addOnce(out.names, n);
            for (auto &n : r.fields)
                addOnce(out.fields, n);
            for (auto *g : r.calls)
                work.push_back(g);
        }
        return out;
    }

    void reads(Expr &e) {
        Reads r;
        scan(e, r, nullptr);
        for (auto &n : r.names)
            watched.insert(n);
        for (auto &n : r.fields)
            fields.insert(n);
        auto deep = called(r.calls);
        for (auto &n : deep.names)
            watchedTop.insert(n);
        for (auto &n : deep.fields)
            fields.insert(n);
    }
    void collect(std::vector<SP> &body) {
        for (auto &s : body) {
            if (s->kind == "when" && s->expr)
                reads(*s->expr);
            if (s->kind == "declare" && s->op == "computed" && s->expr)
                reads(*s->expr);
            if (s->expr)
                collectLambdas(*s->expr);
            if (s->target)
                collectLambdas(*s->target);
            collect(s->body);
            collect(s->other);
            collect(s->finalizer);
            for (auto &c : s->catches)
                collect(c);
        }
    }
    void collectLambdas(Expr &e) {
        for (auto &a : e.args)
            if (a)
                collectLambdas(*a);
        if (e.lambda)
            collect(e.lambda->body);
    }
    bool promoted(const Stmt &s, bool topLevel = false) const {
        auto in = [&](const std::set<std::string> &set) {
            return set.count(s.name) || (!s.alias.empty() && set.count(s.alias));
        };
        return s.op != "const" && (in(watched) || (topLevel && in(watchedTop)));
    }

    Binding *find(const std::string &name) {
        for (auto i = scopes.rbegin(); i != scopes.rend(); ++i)
            if (auto it = i->find(name); it != i->end())
                return &it->second;
        return nullptr;
    }
    static std::string watchers(const std::string &name) {
        return "__fwr_w_" + name;
    }
    static std::string fieldWatchers(const std::string &field) {
        return "__fwr_field_" + field;
    }
    // Runs every watcher in a list; a copy, so watchers may unregister while it runs.
    std::vector<SP> notify(const std::string &list, Location loc) {
        return generated("if " + list + ".length > 0 do\n    for __fwr_f in " + list +
                             ".copy() do\n        __fwr_f()\n    end\nend\n",
                         loc);
    }
    static bool watchable(const Binding *b) {
        return b && (b->kind == Binding::Reactive || b->kind == Binding::Computed);
    }
    // The watcher lists an expression depends on, in order of first use: those of the reactive
    // and computed variables it reads, of the fields it reads, and of what the functions it
    // calls read (top-level variables and fields).
    void dependencies(Expr &e, std::vector<std::string> &out) {
        Reads r;
        scan(e, r, nullptr);
        for (auto &n : r.names)
            if (watchable(find(n)))
                addOnce(out, watchers(n));
        for (auto &f : r.fields)
            if (fields.count(f))
                addOnce(out, fieldWatchers(f));
        auto deep = called(r.calls);
        auto &top = scopes.front();
        for (auto &n : deep.names)
            if (auto it = top.find(n); it != top.end() && watchable(&it->second))
                addOnce(out, watchers(n));
        for (auto &f : deep.fields)
            if (fields.count(f))
                addOnce(out, fieldWatchers(f));
    }
    // After a deferred `when`, every later statement of the block (and of the blocks inside it,
    // but not of functions or lambdas) is followed by a check of the pending flag.
    void settleAfter(std::vector<SP> &list, size_t from, const std::string &pending,
                     const std::string &check, Location loc) {
        for (size_t i = from; i < list.size(); ++i) {
            auto &s = *list[i];
            auto &k = s.kind;
            if (k == "break" || k == "continue" || k == "return" || k == "throw")
                continue;
            if (k != "when" && k != "defer") {
                settleAfter(s.body, 0, pending, check, loc);
                settleAfter(s.other, 0, pending, check, loc);
                settleAfter(s.finalizer, 0, pending, check, loc);
                for (auto &c : s.catches)
                    settleAfter(c, 0, pending, check, loc);
            }
            auto extra = generated("if " + pending + " do\n    " + pending + " = false\n    " +
                                       check + "()\nend\n",
                                   loc);
            list.insert(list.begin() + static_cast<ptrdiff_t>(i + 1), std::move(extra[0]));
            ++i;
        }
    }
    void expression(EP &e) {
        if (!e)
            return;
        if (e->kind == "name")
            if (auto b = find(e->text); b && b->kind == Binding::Alias) {
                auto negated = make("unary", "not", e->loc);
                auto original = make("name", b->target, e->loc);
                original->grouped = true;
                negated->args.push_back(std::move(original));
                negated->grouped = true;
                e = std::move(negated);
                return;
            }
        for (auto &a : e->args)
            expression(a);
        if (e->lambda)
            function(*e->lambda);
    }
    void function(Function &f) {
        scopes.emplace_back();
        for (auto &p : f.params) {
            expression(p.value);
            scopes.back()[p.name] = {};
        }
        block(f.body, false);
        scopes.pop_back();
    }
    void bind(const std::string &name, Binding b) {
        scopes.back()[name] = std::move(b);
    }
    // After a `when` or `computed` inside a block, the rest of the block runs in a try whose
    // finally removes the watchers again.
    void unregisterAtEnd(std::vector<SP> &list, size_t after, const std::string &closure,
                         const std::vector<std::string> &deps, Location loc) {
        std::string text;
        for (auto &d : deps)
            text += d + ".remove(" + closure + ")\n";
        auto t = std::make_unique<Stmt>();
        t->kind = "try";
        t->loc = loc;
        t->finalizer = generated(text, loc);
        for (size_t j = after; j < list.size(); ++j)
            t->body.push_back(std::move(list[j]));
        list.resize(after);
        list.push_back(std::move(t));
    }
    std::vector<std::string> requireDependencies(Expr &e, const std::string &what, Location loc) {
        std::vector<std::string> deps;
        dependencies(e, deps);
        if (deps.empty())
            throw Error(loc, what + " must read a variable or a field, such as `alive` in "
                                    "`when alive do`; parameters and loop variables are not "
                                    "watched");
        return deps;
    }

  public:
    void block(std::vector<SP> &list, bool topLevel) {
        scopes.emplace_back();
        for (size_t i = 0; i < list.size(); ++i) {
            auto &s = *list[i];
            if (s.kind == "declare") {
                expression(s.expr);
                if (s.op == "computed") {
                    // computed T y = expr: y is recomputed (and its watchers run) whenever a
                    // variable that expr reads changes. Assignments to y are rejected.
                    auto deps = requireDependencies(*s.expr, "a computed value", s.loc);
                    auto n = std::to_string(++counter);
                    auto update = "__fwr_update" + n;
                    std::string text = std::string(watcherType) + " " + watchers(s.name) +
                                       " = []\nvar " + update +
                                       " = function() returns void\n    " + s.name +
                                       " = __fwr_value\nend\n";
                    for (auto &d : deps)
                        text += d + ".push(" + update + ")\n";
                    auto extra = generated(text, s.loc);
                    // The update closure assigns the expression, then notifies y's watchers.
                    auto &body = extra[1]->expr->lambda->body;
                    body[0]->expr = clone(*s.expr);
                    for (auto &st : notify(watchers(s.name), s.loc))
                        body.push_back(std::move(st));
                    s.op = "var";
                    bind(s.name, {Binding::Computed, ""});
                    if (!s.alias.empty())
                        bind(s.alias, {Binding::Alias, s.name});
                    s.alias.clear();
                    size_t at = i + 1;
                    for (auto &st : extra)
                        list.insert(list.begin() + static_cast<ptrdiff_t>(at++), std::move(st));
                    if (!topLevel)
                        unregisterAtEnd(list, at, update, deps, list[i]->loc);
                    i = at - 1;
                    continue;
                }
                bool reactive = s.op == "reactive" || promoted(s, topLevel);
                if (s.op == "reactive")
                    s.op = "var";
                bind(s.name, {reactive ? Binding::Reactive : Binding::Plain, ""});
                if (!s.alias.empty())
                    bind(s.alias, {Binding::Alias, s.name});
                s.alias.clear();
                if (reactive) {
                    auto extra =
                        generated(std::string(watcherType) + " " + watchers(s.name) + " = []\n", s.loc);
                    list.insert(list.begin() + static_cast<ptrdiff_t>(i + 1), std::move(extra[0]));
                    ++i;
                }
                continue;
            }
            if (s.kind == "assign") {
                expression(s.expr);
                if (s.target->kind != "name") {
                    expression(s.target);
                    // `x.health = 1` notifies the conditions that read a `.health`; `xs[0] = 1`
                    // and `p.x = 1` notify the watchers of a reactive `xs` or `p`.
                    std::vector<std::string> lists;
                    if ((s.target->kind == "member" || s.target->kind == "inst") &&
                        fields.count(s.target->text))
                        lists.push_back(fieldWatchers(s.target->text));
                    Expr *root = s.target.get();
                    while ((root->kind == "member" || root->kind == "index") && !root->args.empty() &&
                           root->args[0])
                        root = root->args[0].get();
                    if (root->kind == "name") {
                        auto b = find(root->text);
                        if (b && b->kind == Binding::Computed)
                            throw Error(s.loc, "cannot assign into computed value '" + root->text +
                                                   "'; it follows its expression");
                        if (b && b->kind == Binding::Reactive)
                            lists.push_back(watchers(root->text));
                    }
                    size_t at = i + 1;
                    for (auto &l : lists)
                        for (auto &st : notify(l, s.loc))
                            list.insert(list.begin() + static_cast<ptrdiff_t>(at++), std::move(st));
                    i = at - 1;
                    continue;
                }
                auto b = find(s.target->text);
                if (b && b->kind == Binding::Alias) {
                    if (s.op != "=")
                        throw Error(s.loc, "'" + s.target->text + "' is a negated alias of '" +
                                               b->target + "'; assign it with =");
                    auto negated = make("unary", "not", s.expr->loc);
                    s.expr->grouped = true;
                    negated->args.push_back(std::move(s.expr));
                    s.expr = std::move(negated);
                    s.target->text = b->target;
                    b = find(b->target);
                }
                if (b && b->kind == Binding::Computed)
                    throw Error(s.loc, "cannot assign to computed value '" + s.target->text +
                                           "'; it follows its expression");
                if (b && b->kind == Binding::Reactive) {
                    auto extra = notify(watchers(s.target->text), s.loc);
                    list.insert(list.begin() + static_cast<ptrdiff_t>(i + 1), std::move(extra[0]));
                    ++i;
                }
                continue;
            }
            if (s.kind == "when") {
                // when cond do body else do other end: a check closure that runs body on each
                // false -> true change of cond and other on each true -> false change. It is
                // registered with every watcher list cond depends on, and run once now (so a
                // condition that is already true runs body immediately). A deferred when
                // registers a closure that only marks it pending; the check runs between the
                // statements that follow.
                expression(s.expr);
                auto deps = requireDependencies(*s.expr, "a 'when' condition", s.loc);
                bool deferred = s.op == "defer";
                scopes.emplace_back(); // the bodies run inside the check closure
                block(s.body, false);
                scopes.pop_back();
                scopes.emplace_back();
                block(s.other, false);
                scopes.pop_back();
                auto n = std::to_string(++counter);
                auto armed = "__fwr_armed" + n, check = "__fwr_check" + n;
                auto pending = "__fwr_pending" + n, mark = "__fwr_mark" + n;
                std::string text = "var " + armed + " = true\nvar " + check +
                                   " = function() returns void\n"
                                   "    if __fwr_condition do\n"
                                   "        if " + armed + " do\n"
                                   "            " + armed + " = false\n"
                                   "        end\n"
                                   "    else do\n"
                                   "        if not " + armed + " do\n"
                                   "            " + armed + " = true\n"
                                   "        end\n"
                                   "    end\n"
                                   "end\n";
                if (deferred)
                    text += "var " + pending + " = false\nvar " + mark +
                            " = function() returns void\n    " + pending + " = true\nend\n";
                auto watcher = deferred ? mark : check;
                for (auto &d : deps)
                    text += d + ".push(" + watcher + ")\n";
                text += check + "()\n";
                auto loc = s.loc;
                auto extra = generated(text, loc);
                auto &test = *extra[1]->expr->lambda->body[0];
                test.expr = std::move(s.expr);
                for (auto &st : s.body)
                    test.body[0]->body.push_back(std::move(st));
                for (auto &st : s.other)
                    test.other[0]->body.push_back(std::move(st));
                list.erase(list.begin() + static_cast<ptrdiff_t>(i));
                size_t at = i;
                for (auto &st : extra)
                    list.insert(list.begin() + static_cast<ptrdiff_t>(at++), std::move(st));
                bind(armed, {});
                bind(check, {});
                if (deferred) {
                    bind(pending, {});
                    bind(mark, {});
                    settleAfter(list, at, pending, check, loc);
                }
                if (!topLevel)
                    unregisterAtEnd(list, at, watcher, deps, loc);
                i = at - 1;
                continue;
            }
            if (s.kind == "for") {
                expression(s.expr);
                scopes.emplace_back();
                bind(s.name, {});
                if (!s.op.empty())
                    bind(s.op, {});
                block(s.body, false);
                scopes.pop_back();
                continue;
            }
            if (s.kind == "try") {
                block(s.body, false);
                for (size_t c = 0; c < s.catches.size(); ++c) {
                    scopes.emplace_back();
                    if (c < s.catchNames.size())
                        bind(s.catchNames[c], {});
                    block(s.catches[c], false);
                    scopes.pop_back();
                }
                block(s.finalizer, false);
                continue;
            }
            if (s.kind == "match") {
                expression(s.expr);
                for (size_t c = 0; c < s.catches.size(); ++c) {
                    scopes.emplace_back();
                    if (c < s.catchNames.size() && !s.catchNames[c].empty())
                        bind(s.catchNames[c], {});
                    // Bare names in patterns bind values; patterns themselves are left alone.
                    std::function<void(Expr &)> names = [&](Expr &p) {
                        if (p.kind == "name")
                            bind(p.text, {});
                        for (auto &a : p.args)
                            if (a)
                                names(*a);
                    };
                    if (c < s.patterns.size())
                        for (auto &p : s.patterns[c])
                            names(*p);
                    if (c < s.guards.size())
                        expression(s.guards[c]);
                    block(s.catches[c], false);
                    scopes.pop_back();
                }
                block(s.other, false);
                continue;
            }
            expression(s.expr);
            expression(s.target);
            block(s.body, false);
            block(s.other, false);
            block(s.finalizer, false);
        }
        scopes.pop_back();
    }
    void program(Program &p) {
        // Top-level declarations are visible in every function, whatever their position.
        std::map<std::string, Binding> top;
        bool any = false;
        std::function<bool(std::vector<SP> &)> uses = [&](std::vector<SP> &body) {
            for (auto &s : body)
                if ((s->kind == "declare" && (s->op == "reactive" || s->op == "computed" ||
                                              !s->alias.empty())) ||
                    s->kind == "when" || uses(s->body) || uses(s->other) || uses(s->finalizer))
                    return true;
            return false;
        };
        std::function<bool(Expr &)> lambdaUses = [&](Expr &e) {
            for (auto &a : e.args)
                if (a && lambdaUses(*a))
                    return true;
            return e.lambda && uses(e.lambda->body);
        };
        any = uses(p.body);
        for (auto &f : p.functions)
            any = any || uses(f.body);
        for (auto &c : p.classes)
            for (auto &m : c.methods)
                any = any || uses(m.body);
        if (!any) {
            // Lambdas nested in expressions may still declare them.
            std::function<bool(std::vector<SP> &)> deep = [&](std::vector<SP> &body) {
                for (auto &s : body)
                    if ((s->expr && lambdaUses(*s->expr)) || (s->target && lambdaUses(*s->target)) ||
                        deep(s->body) || deep(s->other) || deep(s->finalizer))
                        return true;
                return false;
            };
            any = deep(p.body);
            for (auto &f : p.functions)
                any = any || deep(f.body);
            for (auto &c : p.classes)
                for (auto &m : c.methods)
                    any = any || deep(m.body);
        }
        if (!any)
            return;
        for (auto &f : p.functions)
            functionsByName[f.name].push_back(&f);
        for (auto &c : p.classes)
            for (auto &m : c.methods)
                methodsByName[m.name].push_back(&m);
        collect(p.body);
        for (auto &f : p.functions)
            collect(f.body);
        for (auto &c : p.classes)
            for (auto &m : c.methods)
                collect(m.body);
        for (auto &s : p.body)
            if (s->kind == "declare") {
                top[s->name] = {s->op == "reactive" || promoted(*s, true) ? Binding::Reactive
                                : s->op == "computed" ? Binding::Computed
                                                      : Binding::Plain,
                                ""};
                if (!s->alias.empty())
                    top[s->alias] = {Binding::Alias, s->name};
            }
        // A watcher list for each field a condition reads, before anything can assign one.
        std::string lists;
        for (auto &f : fields)
            lists += std::string(watcherType) + " " + fieldWatchers(f) + " = []\n";
        if (!lists.empty()) {
            auto extra = generated(lists, p.body.empty() ? Location{} : p.body.front()->loc);
            p.body.insert(p.body.begin(), std::make_move_iterator(extra.begin()),
                          std::make_move_iterator(extra.end()));
        }
        scopes.push_back(top);
        for (auto &f : p.functions)
            function(f);
        for (auto &c : p.classes)
            for (auto &m : c.methods)
                function(m);
        scopes.pop_back();
        scopes.push_back(top);
        block(p.body, true);
        scopes.pop_back();
    }
};
} // namespace
void lowerReactive(Program &program) {
    Lowerer().program(program);
}
} // namespace fw
