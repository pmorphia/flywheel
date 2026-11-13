// Native imports: `import c name`, `import cxx name` (`import++ name`), `import rust name`, and
// `import zig name`. The source file is scanned for functions whose parameter and result types
// map to Flywheel (integers -> int, floating point -> double, bool, strings -> string, void).
// A generated shim includes the source and exposes each function as a C symbol with a uniform
// ABI (int64_t, double, const char *, malloc'd char * results); the compiler calls the shims.
// Other pointers cross as int handles (the address). Functions with other types (structs by
// value, function pointers, templates, generics) are not exposed. `// flywheel:` comments in a
// C or C++ source name the libraries and flags it needs (see scanDirectives).
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace fw {
namespace {
namespace fs = std::filesystem;
std::string readText(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
// Splits source into identifiers, numbers, string/char literals (as "" and ''), and punctuation;
// comments and (for C and C++) preprocessor lines are dropped.
std::vector<std::string> tokenize(const std::string &s, const std::string &language) {
    std::vector<std::string> out;
    bool lineStart = true;
    for (size_t p = 0; p < s.size();) {
        char c = s[p];
        if (c == '\n') {
            lineStart = true;
            ++p;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++p;
            continue;
        }
        if ((language == "c" || language == "cxx") && lineStart && c == '#') {
            while (p < s.size() && s[p] != '\n') {
                if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '\n')
                    ++p;
                ++p;
            }
            continue;
        }
        lineStart = false;
        if (s.compare(p, 2, "//") == 0 || (language == "zig" && s.compare(p, 2, "\\\\") == 0)) {
            p = s.find('\n', p);
            if (p == std::string::npos)
                break;
            if (language == "zig" && s.compare(p, 2, "\\\\") != 0)
                out.push_back(""); // keeps multiline strings from merging tokens
            continue;
        }
        if (s.compare(p, 2, "/*") == 0) {
            auto e = s.find("*/", p + 2);
            p = e == std::string::npos ? s.size() : e + 2;
            continue;
        }
        if (c == '"' || (c == '\'' && language != "rust")) {
            size_t q = p + 1;
            while (q < s.size() && s[q] != c && s[q] != '\n') {
                if (s[q] == '\\')
                    ++q;
                ++q;
            }
            // C++ raw strings R"(...)" are rare in function signatures; skipped as plain text.
            out.push_back(std::string(2, c));
            p = q + 1;
            continue;
        }
        if (c == '\'' && language == "rust") {
            // A lifetime ('a) or a char literal ('x', '\n').
            if (p + 2 < s.size() && (s[p + 2] == '\'' || s[p + 1] == '\\')) {
                auto e = s.find('\'', p + 2);
                p = e == std::string::npos ? s.size() : e + 1;
                out.push_back("''");
                continue;
            }
            size_t q = p + 1;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '_'))
                ++q;
            out.push_back(s.substr(p, q - p));
            p = q;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
            static_cast<unsigned char>(c) >= 0x80) {
            size_t q = p;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '_' ||
                                    static_cast<unsigned char>(s[q]) >= 0x80))
                ++q;
            out.push_back(s.substr(p, q - p));
            p = q;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t q = p;
            while (q < s.size() && (std::isalnum(static_cast<unsigned char>(s[q])) || s[q] == '.'))
                ++q;
            out.push_back(s.substr(p, q - p));
            p = q;
            continue;
        }
        for (const char *multi : {"::", "->", "...", "=>"})
            if (s.compare(p, std::strlen(multi), multi) == 0) {
                out.push_back(multi);
                p += std::strlen(multi);
                goto next;
            }
        out.push_back(std::string(1, c));
        ++p;
    next:;
    }
    return out;
}
std::string join(const std::vector<std::string> &tokens, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; ++i) {
        bool word = !tokens[i].empty() &&
                    (std::isalnum(static_cast<unsigned char>(tokens[i][0])) || tokens[i][0] == '_');
        bool previousWord = !out.empty() && (std::isalnum(static_cast<unsigned char>(out.back())) ||
                                             out.back() == '_');
        if (word && previousWord)
            out += ' ';
        out += tokens[i];
    }
    return out;
}
// Splits tokens [from, to) at top-level commas.
std::vector<std::pair<size_t, size_t>> commaSplit(const std::vector<std::string> &t, size_t from,
                                                  size_t to) {
    std::vector<std::pair<size_t, size_t>> parts;
    int depth = 0;
    size_t start = from;
    for (size_t i = from; i < to; ++i) {
        if (t[i] == "(" || t[i] == "[" || t[i] == "{" || t[i] == "<")
            ++depth;
        else if (t[i] == ")" || t[i] == "]" || t[i] == "}" || t[i] == ">")
            --depth;
        else if (t[i] == "," && depth == 0) {
            parts.emplace_back(start, i);
            start = i + 1;
        }
    }
    if (start < to)
        parts.emplace_back(start, to);
    return parts;
}
size_t matching(const std::vector<std::string> &t, size_t open) {
    std::string a = t[open], z = a == "(" ? ")" : a == "{" ? "}" : a == "[" ? "]" : ">";
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i] == a)
            ++depth;
        else if (t[i] == z && --depth == 0)
            return i;
    }
    return t.size();
}
// --- C and C++ -----------------------------------------------------------------------------
// The Flywheel type of a C or C++ type ("" when unsupported). `owned` reports a std::string.
std::string cType(std::vector<std::string> words, bool cxx, bool parameter, std::string &kind) {
    std::vector<std::string> kept;
    for (auto &w : words)
        if (w != "const" && w != "volatile" && w != "register" && w != "restrict" &&
            w != "__restrict" && w != "__restrict__" && w != "inline" && w != "static" &&
            w != "extern" && w != "constexpr" && w != "typename" && w != "\"\"" && w != "noexcept")
            kept.push_back(w);
    auto text = join(kept, 0, kept.size());
    if (text.rfind("std::", 0) == 0 && text.find("string") == std::string::npos)
        text = text.substr(5);
    static const std::set<std::string> integers = {"int",
                                                   "long",
                                                   "short",
                                                   "unsigned",
                                                   "signed",
                                                   "long long",
                                                   "unsigned int",
                                                   "signed int",
                                                   "unsigned long",
                                                   "long int",
                                                   "unsigned long long",
                                                   "long long int",
                                                   "unsigned long int",
                                                   "short int",
                                                   "unsigned short",
                                                   "unsigned short int",
                                                   "signed char",
                                                   "unsigned char",
                                                   "signed long",
                                                   "signed short",
                                                   "int8_t",
                                                   "int16_t",
                                                   "int32_t",
                                                   "int64_t",
                                                   "uint8_t",
                                                   "uint16_t",
                                                   "uint32_t",
                                                   "uint64_t",
                                                   "size_t",
                                                   "ssize_t",
                                                   "ptrdiff_t",
                                                   "intptr_t",
                                                   "uintptr_t",
                                                   "intmax_t",
                                                   "uintmax_t",
                                                   "long unsigned",
                                                   "long unsigned int"};
    if (integers.count(text)) {
        kind = "int";
        return "int";
    }
    if (text == "float" || text == "double" || text == "long double") {
        kind = "real";
        return "double";
    }
    if (text == "bool" || text == "_Bool") {
        kind = "bool";
        return "bool";
    }
    if (text == "void") {
        kind = "void";
        return parameter ? "" : "void";
    }
    if (text == "char*") {
        bool constant = std::find(words.begin(), words.end(), "const") != words.end();
        kind = constant ? "cstr" : "mutable-cstr";
        return "string";
    }
    // Any other pointer (`void *`, `Texture *`, `struct node **`) crosses as an int handle
    // holding the address; 0 is the null pointer.
    if (text.size() > 1 && text.back() == '*' && text.find('(') == std::string::npos &&
        text.find('[') == std::string::npos && text.find('<') == std::string::npos &&
        text.find('&') == std::string::npos) {
        kind = "pointer";
        return "int";
    }
    if (cxx && (text == "std::string" || text == "string" || text == "std::string_view" ||
                text == "string_view" || text == "std::string&" || text == "string&")) {
        bool reference = text.back() == '&';
        bool constant = std::find(words.begin(), words.end(), "const") != words.end();
        if (reference && (!constant || !parameter))
            return ""; // a mutable reference cannot bind a temporary
        kind = text.find("view") != std::string::npos ? "view" : "std";
        return "string";
    }
    return "";
}
// --- C structs, pointers to elements, and callbacks ------------------------------------------
// Words of a C type without qualifiers and the `struct` keyword.
std::vector<std::string> bareWords(const std::vector<std::string> &words) {
    static const std::set<std::string> dropped = {"const",    "volatile", "struct",  "restrict",
                                                  "__restrict", "__restrict__", "register",
                                                  "static", "extern", "inline", "typename"};
    std::vector<std::string> kept;
    for (auto &w : words)
        if (!dropped.count(w))
            kept.push_back(w);
    return kept;
}
// The Flywheel type of a C scalar held in memory (a struct field or a pointer's element), with
// its exact size: "i32" for int, "float" for float. "" when it is not a scalar.
std::string scalarType(const std::string &text) {
    static const std::map<std::string, std::string> types = {
        {"float", "float"},          {"double", "double"},        {"bool", "bool"},
        {"_Bool", "bool"},           {"char", "i8"},              {"signed char", "i8"},
        {"int8_t", "i8"},            {"unsigned char", "u8"},     {"uint8_t", "u8"},
        {"short", "i16"},            {"short int", "i16"},        {"signed short", "i16"},
        {"int16_t", "i16"},          {"unsigned short", "u16"},   {"unsigned short int", "u16"},
        {"uint16_t", "u16"},         {"int", "i32"},              {"signed", "i32"},
        {"signed int", "i32"},       {"int32_t", "i32"},          {"unsigned", "u32"},
        {"unsigned int", "u32"},     {"uint32_t", "u32"},         {"long long", "i64"},
        {"long long int", "i64"},    {"int64_t", "i64"},          {"ssize_t", "i64"},
        {"ptrdiff_t", "i64"},        {"intptr_t", "i64"},         {"intmax_t", "i64"},
        {"unsigned long long", "u64"}, {"unsigned long long int", "u64"}, {"uint64_t", "u64"},
        {"size_t", "u64"},           {"uintptr_t", "u64"},        {"uintmax_t", "u64"},
#ifdef _WIN32
        {"long", "i32"},             {"long int", "i32"},         {"signed long", "i32"},
        {"unsigned long", "u32"},    {"unsigned long int", "u32"}, {"long unsigned", "u32"},
#else
        {"long", "i64"},             {"long int", "i64"},         {"signed long", "i64"},
        {"unsigned long", "u64"},    {"unsigned long int", "u64"}, {"long unsigned", "u64"},
#endif
    };
    auto found = types.find(text);
    return found == types.end() ? "" : found->second;
}
const ForeignStruct *findStruct(const ForeignModule &m, const std::string &name) {
    for (auto &s : m.structs)
        if (s.name == name)
            return &s;
    return nullptr;
}
bool reservedName(const std::string &name);
// The fields of a struct body (the tokens between its braces); false when a member is not
// something a Flywheel struct can hold.
bool structFields(const std::vector<std::string> &t, size_t from, size_t to, const ForeignModule &m,
                  const std::map<std::string, std::string> &tags,
                  std::vector<std::pair<std::string, std::string>> &fields) {
    size_t start = from;
    for (size_t i = from; i <= to; ++i) {
        if (i < to && t[i] != ";")
            continue;
        std::vector<std::string> member(t.begin() + static_cast<ptrdiff_t>(start),
                                        t.begin() + static_cast<ptrdiff_t>(i));
        start = i + 1;
        if (member.empty())
            continue;
        for (auto &w : member)
            if (w == "(" || w == ":" || w == "{" || w == "union" || w == "=")
                return false;
        auto pieces = commaSplit(member, 0, member.size());
        std::vector<std::string> base;
        for (size_t p = 0; p < pieces.size(); ++p) {
            std::vector<std::string> d(member.begin() + static_cast<ptrdiff_t>(pieces[p].first),
                                       member.begin() + static_cast<ptrdiff_t>(pieces[p].second));
            size_t count = 1;
            while (d.size() >= 3 && d.back() == "]") {
                auto &n = d[d.size() - 2];
                if (d[d.size() - 3] != "[" || n.empty() ||
                    !std::all_of(n.begin(), n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                    return false;
                count *= static_cast<size_t>(std::stoul(n));
                d.resize(d.size() - 3);
            }
            if (d.empty() || !(std::isalpha(static_cast<unsigned char>(d.back()[0])) || d.back()[0] == '_'))
                return false;
            auto name = d.back();
            d.pop_back();
            size_t stars = 0;
            while (!d.empty() && d.back() == "*") {
                ++stars;
                d.pop_back();
            }
            if (p == 0)
                base = d;
            auto words = bareWords(base);
            std::string type;
            if (stars)
                type = "i64"; // a pointer: an int handle
            else if (words.size() == 1 && findStruct(m, words[0]))
                type = "struct:" + words[0];
            else if (words.size() == 1 && tags.count(words[0]))
                type = "struct:" + tags.at(words[0]);
            else
                type = scalarType(join(words, 0, words.size()));
            if (type.empty() || reservedName(name) || count == 0 || count > 4096)
                return false;
            if (count == 1)
                fields.push_back({name, type});
            else
                for (size_t k = 0; k < count; ++k)
                    fields.push_back({name + std::to_string(k), type});
        }
    }
    return !fields.empty();
}
// A function pointer type `R (*)(params)`: the closure types it can take, or false.
bool makeCallback(const std::vector<std::string> &resultWords, const std::vector<std::string> &t,
                  size_t from, size_t to, bool cxx, ForeignCallback &out) {
    std::string kind;
    auto result = cType(resultWords, cxx, false, kind);
    if (kind == "void")
        out.result = "void";
    else if (kind == "int" || kind == "pointer")
        out.result = "i64";
    else if (kind == "real")
        out.result = "double";
    else
        return false; // bool, string, or unsupported results
    {
        std::vector<std::string> kept;
        for (auto &w : resultWords)
            if (w != "static" && w != "extern" && w != "inline")
                kept.push_back(w);
        out.cResult = join(kept, 0, kept.size());
    }
    auto params = commaSplit(t, from, to);
    if (params.size() == 1 && params[0].second - params[0].first == 1 && t[params[0].first] == "void")
        params.clear();
    for (auto [a, z] : params) {
        std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(a),
                                       t.begin() + static_cast<ptrdiff_t>(z));
        std::string k;
        auto type = cType(words, cxx, true, k);
        if (type.empty() && words.size() > 1) {
            words.pop_back();
            type = cType(words, cxx, true, k);
        }
        if (k == "int" || k == "pointer")
            out.params.push_back("i64");
        else if (k == "real")
            out.params.push_back("double");
        else if (k == "bool")
            out.params.push_back("bool");
        else
            return false;
        out.cParams.push_back(join(words, 0, words.size()));
    }
    return true;
}
std::string callbackType(const ForeignCallback &c) {
    std::string type = "function<" + c.result;
    for (auto &p : c.params)
        type += "," + p;
    return type + ">";
}
// The index of an equal callback in the module, adding it if new.
size_t addCallback(ForeignModule &m, ForeignCallback c) {
    for (size_t i = 0; i < m.callbacks.size(); ++i)
        if (m.callbacks[i].cType == c.cType && m.callbacks[i].params == c.params &&
            m.callbacks[i].result == c.result)
            return i;
    c.symbol = "fw_" + m.language + "_" + m.key + "_callback" + std::to_string(m.callbacks.size());
    m.callbacks.push_back(std::move(c));
    return m.callbacks.size() - 1;
}
// Struct definitions and function pointer typedefs at the top level of a C or C++ source.
struct CTypes {
    std::map<std::string, std::string> tags;            // struct Tag -> its typedef name
    std::map<std::string, ForeignCallback> callbacks;   // typedef name -> callback
};
CTypes scanTypes(const std::vector<std::string> &t, bool cxx, ForeignModule &m) {
    CTypes types;
    auto addStruct = [&](const std::string &name, size_t open, size_t close) {
        ForeignStruct s;
        s.name = name;
        if (!findStruct(m, name) && !reservedName(name) &&
            structFields(t, open + 1, close, m, types.tags, s.fields))
            m.structs.push_back(std::move(s));
    };
    for (size_t i = 0; i < t.size();) {
        if (t[i] == "typedef") {
            size_t end = i + 1;
            while (end < t.size() && t[end] != ";")
                end = t[end] == "{" ? matching(t, end) + 1 : end + 1;
            if (end >= t.size())
                break;
            if (t[i + 1] == "struct") {
                size_t open = i + 2;
                std::string tag;
                if (open < end && t[open] != "{")
                    tag = t[open++];
                if (open < end && t[open] == "{") {
                    auto close = matching(t, open);
                    if (close + 2 == end) {
                        addStruct(t[close + 1], open, close);
                        if (!tag.empty() && findStruct(m, t[close + 1]))
                            types.tags[tag] = t[close + 1];
                    }
                } else if (!tag.empty() && open + 1 == end) {
                    // typedef struct Tag Name;
                    if (auto known = findStruct(m, tag)) {
                        ForeignStruct alias = *known;
                        alias.name = t[open];
                        if (!findStruct(m, alias.name))
                            m.structs.push_back(alias);
                    } else if (types.tags.count(tag))
                        types.tags[t[open]] = types.tags.at(tag);
                }
            } else {
                // typedef R (*Name)(params);
                size_t k = i + 1;
                while (k < end && t[k] != "(")
                    ++k;
                if (k + 4 < end && t[k + 1] == "*" && t[k + 3] == ")" && t[k + 4] == "(") {
                    auto close = matching(t, k + 4);
                    std::vector<std::string> resultWords(t.begin() + static_cast<ptrdiff_t>(i + 1),
                                                         t.begin() + static_cast<ptrdiff_t>(k));
                    ForeignCallback c;
                    c.cType = t[k + 2];
                    if (close + 1 == end && makeCallback(resultWords, t, k + 5, close, cxx, c))
                        types.callbacks[t[k + 2]] = c;
                }
            }
            i = end + 1;
            continue;
        }
        if (t[i] == "struct" && i + 2 < t.size() && t[i + 2] == "{") {
            auto close = matching(t, i + 2);
            if (close + 1 < t.size() && t[close + 1] == ";") {
                addStruct(t[i + 1], i + 2, close);
                if (findStruct(m, t[i + 1]))
                    types.tags[t[i + 1]] = t[i + 1];
            }
            i = close + 1;
            continue;
        }
        if (t[i] == "{") {
            i = matching(t, i) + 1;
            continue;
        }
        ++i;
    }
    return types;
}
// The Flywheel type of a parameter that is a struct, a function pointer, or a pointer to
// elements, beyond what cType knows. Sets `kind` ("struct", "callback", or as cType),
// `spelled` (the C type without the parameter's name), `pointee`, and `callback`.
std::string paramType(std::vector<std::string> words, bool cxx, ForeignModule &m,
                      const CTypes &types, std::string &kind, std::string &spelled,
                      std::string &pointee, std::string &callback) {
    // An inline function pointer: R (*name)(params)
    for (size_t k = 0; k + 2 < words.size(); ++k)
        if (words[k] == "(" && words[k + 1] == "*") {
            auto close = matching(words, k);
            if (close + 1 >= words.size() || words[close + 1] != "(")
                return "";
            auto end = matching(words, close + 1);
            std::vector<std::string> resultWords(words.begin(), words.begin() + static_cast<ptrdiff_t>(k));
            ForeignCallback c;
            if (!makeCallback(resultWords, words, close + 2, end, cxx, c))
                return "";
            std::vector<std::string> params(words.begin() + static_cast<ptrdiff_t>(close + 2),
                                            words.begin() + static_cast<ptrdiff_t>(end));
            c.cType = join(resultWords, 0, resultWords.size()) + " (*)(" + join(params, 0, params.size()) + ")";
            auto index = addCallback(m, c);
            kind = "callback";
            spelled = m.callbacks[index].cType;
            callback = m.callbacks[index].symbol;
            return callbackType(m.callbacks[index]);
        }
    for (int drop = 0; drop < 2 && !words.empty(); ++drop) {
        if (drop)
            words.pop_back(); // the parameter's name
        auto bare = bareWords(words);
        std::vector<std::string> noConst;
        for (auto &w : words)
            if (w != "const" && w != "volatile")
                noConst.push_back(w);
        if (bare.size() == 1) {
            auto name = types.tags.count(bare[0]) ? types.tags.at(bare[0]) : bare[0];
            if (findStruct(m, name) && (name == bare[0] || types.tags.count(bare[0]))) {
                kind = "struct";
                spelled = join(noConst, 0, noConst.size());
                return "struct:" + name;
            }
            if (auto found = types.callbacks.find(bare[0]); found != types.callbacks.end()) {
                auto index = addCallback(m, found->second);
                kind = "callback";
                spelled = bare[0];
                callback = m.callbacks[index].symbol;
                return callbackType(m.callbacks[index]);
            }
        }
        auto type = cType(words, cxx, true, kind);
        if (type.empty())
            continue;
        spelled = join(words, 0, words.size());
        if (kind == "pointer" && bare.size() >= 2 && bare.back() == "*" &&
            std::count(bare.begin(), bare.end(), "*") == 1) {
            auto element = join(bare, 0, bare.size() - 1);
            auto tagged = types.tags.count(element) ? types.tags.at(element) : element;
            if (element == "void")
                pointee = "void";
            else if (findStruct(m, tagged))
                pointee = "struct:" + tagged;
            else
                pointee = scalarType(element);
        }
        return type;
    }
    return "";
}
void scanC(const std::string &source, bool cxx, ForeignModule &m) {
    auto t = tokenize(source, cxx ? "cxx" : "c");
    auto types = scanTypes(t, cxx, m);
    size_t start = 0; // first token of the current top-level declaration
    std::function<void(size_t, size_t)> scope = [&](size_t from, size_t to) {
        start = from;
        for (size_t i = from; i < to; ++i) {
            if (t[i] == ";" || t[i] == "}") {
                start = i + 1;
                continue;
            }
            if (t[i] == "(") {
                // `[ret] name ( params ) [qualifiers] {` at this level defines a function.
                auto close = matching(t, i);
                if (close >= to)
                    return;
                size_t body = close + 1;
                while (body < to && (t[body] == "const" || t[body] == "noexcept" ||
                                     t[body] == "override" || t[body] == "final"))
                    ++body;
                if (body < to && t[body] == "{" && i > start) {
                    auto name = t[i - 1];
                    bool qualified = i >= 2 && t[i - 2] == "::";
                    std::vector<std::string> head(t.begin() + static_cast<ptrdiff_t>(start),
                                                  t.begin() + static_cast<ptrdiff_t>(i - 1));
                    bool templated = std::find(head.begin(), head.end(), "template") != head.end();
                    bool word = std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_';
                    std::vector<std::string> returnWords;
                    for (size_t k = 0; k < head.size(); ++k) {
                        if (head[k] == "[" && k + 1 < head.size() && head[k + 1] == "[") {
                            while (k < head.size() && head[k] != "]")
                                ++k;
                            ++k; // [[attributes]]
                            continue;
                        }
                        if (head[k] == "__attribute__" || head[k] == "__declspec")
                            break;
                        returnWords.push_back(head[k]);
                    }
                    std::string kind;
                    ForeignFunction f;
                    f.name = name;
                    bool candidate =
                        word && !qualified && !templated && name != "main" && name != "operator";
                    f.result = candidate ? cType(returnWords, cxx, false, kind) : "";
                    if (candidate && f.result.empty()) {
                        auto bare = bareWords(returnWords);
                        if (bare.size() == 1) {
                            auto structName = types.tags.count(bare[0]) ? types.tags.at(bare[0]) : bare[0];
                            if (findStruct(m, structName)) {
                                std::vector<std::string> spelled;
                                for (auto &w : returnWords)
                                    if (w != "const" && w != "static" && w != "inline" && w != "extern")
                                        spelled.push_back(w);
                                f.result = "struct:" + structName;
                                kind = "struct:" + join(spelled, 0, spelled.size());
                            }
                        }
                    }
                    f.nativeResult = kind;
                    bool ok = !f.result.empty();
                    auto params = commaSplit(t, i + 1, close);
                    if (params.size() == 1 && params[0].second - params[0].first == 1 &&
                        t[params[0].first] == "void")
                        params.clear();
                    for (auto [a, z] : params) {
                        if (!ok)
                            break;
                        for (size_t k = a; k < z; ++k)
                            if (t[k] == "=") {
                                z = k; // default argument
                                break;
                            }
                        std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(a),
                                                       t.begin() + static_cast<ptrdiff_t>(z));
                        std::string paramKind, spelled, pointee, callback;
                        auto type = paramType(words, cxx, m, types, paramKind, spelled, pointee,
                                              callback);
                        if (type.empty())
                            ok = false;
                        f.params.push_back(type);
                        f.nativeParams.push_back(paramKind + "|" + spelled);
                        f.pointees.push_back(pointee);
                        f.callbacks.push_back(callback);
                    }
                    if (ok)
                        m.functions.push_back(std::move(f));
                    auto end = matching(t, body);
                    i = end;
                    start = end + 1;
                    continue;
                }
                i = close;
                continue;
            }
            if (t[i] == "{") {
                auto end = matching(t, i);
                // extern "C" { ... } blocks are transparent; namespaces, classes, and
                // initializers are skipped.
                bool externBlock = i >= 2 && t[i - 1] == "\"\"" && t[i - 2] == "extern";
                if (externBlock) {
                    auto saved = end;
                    scope(i + 1, std::min(end, to));
                    i = saved;
                } else
                    i = end;
                start = i + 1;
            }
        }
    };
    scope(0, t.size());
}
std::string cShim(const ForeignModule &m, const std::string &path, bool cxx) {
    std::string s = cxx ? "#include <cstdint>\n#include <cstdlib>\n#include <cstring>\n#include "
                          "<exception>\n#include <string>\n#include <string_view>\n"
                        : "#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
                          "#include <stdbool.h>\n";
    auto generic = fs::path(path).generic_string();
    std::string escaped;
    for (char c : generic)
        escaped += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    s += "#include \"" + escaped + "\"\n";
    std::string linkage = cxx ? "extern \"C\" " : "";
    s += linkage + "void fw_foreign_fail(const char *message);\n";
    s += "static char *fw_shim_copy(const char *text, size_t length) {\n"
         "    char *copy = (char *)malloc(length + 1);\n"
         "    if (copy) {\n        memcpy(copy, text, length);\n        copy[length] = 0;\n    }\n"
         "    return copy;\n}\n";
    if (cxx)
        s += "static char *fw_shim_out(std::string_view text) { return fw_shim_copy(text.data(), "
             "text.size()); }\nstatic char *fw_shim_out(const char *text) { return text ? "
             "fw_shim_copy(text, std::strlen(text)) : fw_shim_copy(\"\", 0); }\n";
    else
        s += "static char *fw_shim_out(const char *text) { return text ? fw_shim_copy(text, "
             "strlen(text)) : fw_shim_copy(\"\", 0); }\n";
    // Callbacks: a fixed table of trampolines per function pointer type. A closure bound to one
    // stays alive for the rest of the program; closures without captured values are the same
    // callback whenever their code is, so a named function passed again reuses its slot.
    if (!m.callbacks.empty())
        s += linkage + "void fw_retain(void *object);\n" + linkage +
             "int64_t fw_length(void *object);\n"
             "typedef struct { void *code; void *closure; } fw_callback_slot;\n";
    const int slots = 64;
    for (auto &c : m.callbacks) {
        auto table = c.symbol + "_slots";
        s += "static fw_callback_slot " + table + "[" + std::to_string(slots) + "];\n";
        auto flyResult = c.result == "void" ? "void" : c.result == "double" ? "double" : "int64_t";
        std::string codeType = std::string(flyResult) + " (*)(void *";
        for (auto &p : c.params)
            codeType += p == "double" ? ", double" : p == "bool" ? ", bool" : ", int64_t";
        codeType += ")";
        for (int k = 0; k < slots; ++k) {
            std::string params, args;
            for (size_t i = 0; i < c.params.size(); ++i) {
                auto a = "p" + std::to_string(i);
                params += (i ? ", " : "") + c.cParams[i] + " " + a;
                auto &p = c.params[i];
                bool pointer = c.cParams[i].find('*') != std::string::npos;
                args += ", " + (p == "double" ? "(double)" + a
                                : p == "bool" ? "(bool)" + a
                                : pointer     ? "(int64_t)(intptr_t)" + a
                                              : "(int64_t)" + a);
            }
            auto name = c.symbol + "_" + std::to_string(k);
            auto slot = table + "[" + std::to_string(k) + "]";
            auto invocation = "((" + codeType + ")" + slot + ".code)(" + slot + ".closure" + args + ")";
            bool pointerResult = c.cResult.find('*') != std::string::npos;
            s += "static " + c.cResult + " " + name + "(" + (params.empty() ? "void" : params) +
                 ") {\n    " +
                 (c.result == "void" ? invocation + ";"
                  : pointerResult    ? "return (" + c.cResult + ")(intptr_t)" + invocation + ";"
                                     : "return (" + c.cResult + ")" + invocation + ";") +
                 "\n}\n";
        }
        s += "static void *" + table + "_code[" + std::to_string(slots) + "] = {";
        for (int k = 0; k < slots; ++k)
            s += std::string(k ? ", " : "") + "(void *)" + c.symbol + "_" + std::to_string(k);
        s += "};\n";
        s += linkage + "int64_t " + c.symbol + "(void *code, void *closure) {\n"
             "    int shared = fw_length(closure) == 0;\n"
             "    for (int i = 0; i < " + std::to_string(slots) + "; i++)\n"
             "        if (" + table + "[i].closure == closure || (shared && " + table +
             "[i].code == code && fw_length(" + table + "[i].closure) == 0))\n"
             "            return (int64_t)(intptr_t)" + table + "_code[i];\n"
             "    for (int i = 0; i < " + std::to_string(slots) + "; i++)\n"
             "        if (!" + table + "[i].closure) {\n"
             "            fw_retain(closure);\n"
             "            " + table + "[i].code = code;\n"
             "            " + table + "[i].closure = closure;\n"
             "            return (int64_t)(intptr_t)" + table + "_code[i];\n"
             "        }\n"
             "    fw_foreign_fail(\"more than " + std::to_string(slots) +
             " different functions passed as one callback type; pass the same function value "
             "again instead of a new lambda\");\n"
             "    return 0;\n}\n";
    }
    auto abi = [](const std::string &type) {
        return type == "int" || type == "bool" ? "int64_t"
               : type == "double"              ? "double"
               : type == "string"              ? "char *"
                                               : "void";
    };
    for (auto &f : m.functions) {
        std::string params, args;
        for (size_t i = 0; i < f.params.size(); ++i) {
            auto a = "a" + std::to_string(i);
            auto &native = f.nativeParams[i];
            auto kind = native.substr(0, native.find('|'));
            auto spelled = native.substr(native.find('|') + 1);
            std::string unconst;
            {
                auto words = tokenize(spelled, cxx ? "cxx" : "c");
                std::vector<std::string> kept;
                for (auto &w : words)
                    if (w != "const" && w != "&")
                        kept.push_back(w);
                unconst = join(kept, 0, kept.size());
            }
            std::string paramType = f.params[i] == "string" ? "const char *"
                                    : kind == "struct"     ? "const " + spelled + " *"
                                    : kind == "callback"   ? "int64_t"
                                                           : abi(f.params[i]);
            params += std::string(params.empty() ? "" : ", ") + paramType + " " + a;
            std::string arg;
            if (kind == "struct")
                arg = "*" + a;
            else if (kind == "callback")
                arg = "(" + spelled + ")(intptr_t)" + a;
            else if (kind == "bool")
                arg = a + " != 0";
            else if (kind == "int" || kind == "real")
                arg = "(" + unconst + ")" + a;
            else if (kind == "pointer")
                arg = "(" + spelled + ")(intptr_t)" + a; // qualifiers kept: `const char **`
            else if (kind == "cstr")
                arg = a;
            else if (kind == "mutable-cstr")
                arg = "(char *)" + a;
            else if (kind == "view")
                arg = "std::string_view(" + a + ")";
            else
                arg = "std::string(" + a + ")";
            args += (args.empty() ? "" : ", ") + arg;
        }
        auto invocation = f.name + "(" + args + ")";
        std::string body;
        bool structResult = f.nativeResult.rfind("struct:", 0) == 0;
        if (structResult) {
            // The result goes to memory the caller provides.
            params = f.nativeResult.substr(7) + " *out" + (params.empty() ? "" : ", " + params);
            body = "*out = " + invocation + ";";
        } else if (f.result == "void")
            body = invocation + ";";
        else if (f.result == "bool")
            body = "return (" + invocation + ") ? 1 : 0;";
        else if (f.result == "string")
            body = "return fw_shim_out(" + invocation + ");";
        else if (f.nativeResult == "pointer")
            body = "return (int64_t)(intptr_t)(" + invocation + ");";
        else
            body = std::string("return (") + abi(f.result) + ")(" + invocation + ");";
        s += linkage + (structResult ? "void" : abi(f.result)) + " " + f.symbol + "(" +
             (params.empty() && !cxx ? "void" : params) + ") {\n";
        if (cxx) {
            s += "    try {\n        " + body +
                 "\n    } catch (const std::exception &e) {\n"
                 "        fw_foreign_fail(e.what());\n    } catch (...) {\n"
                 "        fw_foreign_fail(\"C++ exception\");\n    }\n";
            if (f.result != "void" && !structResult)
                s += std::string("    return ") + (f.result == "string" ? "nullptr" : "0") + ";\n";
        } else
            s += "    " + body + "\n";
        s += "}\n";
    }
    return s;
}
// --- Rust ----------------------------------------------------------------------------------
std::string rustType(const std::vector<std::string> &words, bool parameter, std::string &kind) {
    std::vector<std::string> kept;
    for (auto &w : words)
        if (!(w.size() > 1 && w[0] == '\'')) // lifetimes
            kept.push_back(w);
    auto text = join(kept, 0, kept.size());
    static const std::set<std::string> integers = {"i8",  "i16", "i32", "i64",  "i128",  "u8",
                                                   "u16", "u32", "u64", "u128", "isize", "usize"};
    if (integers.count(text)) {
        kind = "int";
        return "int";
    }
    if (text == "f32" || text == "f64") {
        kind = "real";
        return "double";
    }
    if (text == "bool") {
        kind = "bool";
        return "bool";
    }
    if (text == "&str" || text == "String" || text == "&String") {
        if (parameter && text == "&String")
            kind = "string-ref";
        else
            kind = text == "String" ? "owned" : "str";
        return "string";
    }
    if (!parameter && (text.empty() || text == "()")) {
        kind = "void";
        return "void";
    }
    return "";
}
void scanRust(const std::string &source, ForeignModule &m) {
    auto t = tokenize(source, "rust");
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "{") {
            i = matching(t, i); // impl blocks, nested modules, and bodies
            continue;
        }
        if (t[i] != "fn" || i + 2 >= t.size())
            continue;
        // Visibility: `pub` (or `pub(crate)`) before `fn`, possibly with `extern "C"`.
        size_t k = i;
        if (k >= 2 && t[k - 1] == "\"\"" && t[k - 2] == "extern")
            k -= 2;
        bool visible = (k >= 1 && t[k - 1] == "pub") ||
                       (k >= 4 && t[k - 1] == ")" && t[k - 3] == "(" && t[k - 4] == "pub");
        bool unsafeFn = k >= 1 && t[k - 1] == "unsafe";
        auto name = t[i + 1];
        size_t open = i + 2;
        if (!visible || unsafeFn || t[open] != "(") {
            continue; // generic (`fn f<T>`) or private functions are not exposed
        }
        auto close = matching(t, open);
        size_t body = close + 1;
        std::vector<std::string> result;
        if (body < t.size() && t[body] == "->")
            for (++body; body < t.size() && t[body] != "{" && t[body] != "where"; ++body)
                result.push_back(t[body]);
        if (body >= t.size() || t[body] != "{")
            continue;
        ForeignFunction f;
        f.name = name;
        std::string kind;
        f.result = name == "main" ? "" : rustType(result, false, kind);
        f.nativeResult = kind;
        bool ok = !f.result.empty();
        for (auto [a, z] : commaSplit(t, open + 1, close)) {
            size_t colon = a;
            while (colon < z && t[colon] != ":")
                ++colon;
            if (colon == z) {
                ok = false; // self
                break;
            }
            std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(colon + 1),
                                           t.begin() + static_cast<ptrdiff_t>(z));
            std::string paramKind;
            auto type = rustType(words, true, paramKind);
            if (type.empty()) {
                ok = false;
                break;
            }
            f.params.push_back(type);
            f.nativeParams.push_back(paramKind + "|" + join(words, 0, words.size()));
        }
        if (ok)
            m.functions.push_back(std::move(f));
        i = matching(t, body);
    }
}
std::string rustShim(const ForeignModule &m, const std::string &path) {
    auto module = "fw_module_" + m.key;
    std::string s;
    std::string escaped;
    for (char c : fs::path(path).generic_string())
        escaped += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    s += "#[path = \"" + escaped + "\"]\nmod " + module + ";\n";
    for (auto &f : m.functions) {
        std::string params, args;
        for (size_t i = 0; i < f.params.size(); ++i) {
            auto a = "a" + std::to_string(i);
            auto kind = f.nativeParams[i].substr(0, f.nativeParams[i].find('|'));
            auto spelled = f.nativeParams[i].substr(f.nativeParams[i].find('|') + 1);
            auto abi = f.params[i] == "string"   ? "*const c_char"
                       : f.params[i] == "double" ? "f64"
                                                 : "i64";
            params += (params.empty() ? "" : ", ") + a + ": " + abi;
            std::string arg;
            if (kind == "bool")
                arg = a + " != 0";
            else if (kind == "int" || kind == "real")
                arg = a + " as " + spelled;
            else if (kind == "str")
                arg = "fw_in(" + a + ")";
            else if (kind == "owned")
                arg = "fw_in(" + a + ").to_string()";
            else
                arg = "&fw_in(" + a + ").to_string()";
            args += (args.empty() ? "" : ", ") + arg;
        }
        auto abiResult = f.result == "string"   ? " -> *mut c_char"
                         : f.result == "double" ? " -> f64"
                         : f.result == "void"   ? ""
                                                : " -> i64";
        auto convert = f.result == "void"   ? std::string("")
                       : f.result == "bool" ? std::string("if r { 1 } else { 0 }")
                       : f.result == "string"
                           ? std::string(f.nativeResult == "owned" ? "fw_out(&r)" : "fw_out(r)")
                       : f.result == "double" ? std::string("r as f64")
                                              : std::string("r as i64");
        auto failed = f.result == "void"     ? "()"
                      : f.result == "string" ? "std::ptr::null_mut()"
                      : f.result == "double" ? "0.0"
                                             : "0";
        s += "#[no_mangle]\npub extern \"C\" fn " + f.symbol + "(" + params + ")" + abiResult +
             " {\n    fw_quiet();\n    match "
             "std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| " +
             module + "::" + f.name + "(" + args + "))) {\n        Ok(r) => { " +
             (f.result == "void" ? "let _ = r;" : convert) +
             " }\n        Err(e) => { fw_panic(e); " + failed + " }\n    }\n}\n";
    }
    return s;
}
const char *rustPrelude =
    "#![allow(dead_code, unused_imports, unused_variables, unused_mut, non_snake_case)]\n"
    "use std::os::raw::c_char;\n"
    "extern \"C\" {\n    fn fw_foreign_fail(message: *const c_char);\n"
    "    fn malloc(size: usize) -> *mut u8;\n}\n"
    "fn fw_in(p: *const c_char) -> &'static str {\n"
    "    if p.is_null() { return \"\"; }\n"
    "    unsafe { std::ffi::CStr::from_ptr(p) }.to_str().unwrap_or(\"\")\n}\n"
    "fn fw_out(s: &str) -> *mut c_char {\n"
    "    unsafe {\n        let p = malloc(s.len() + 1);\n        if p.is_null() { return p as *mut "
    "c_char; }\n"
    "        std::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len());\n"
    "        *p.add(s.len()) = 0;\n        p as *mut c_char\n    }\n}\n"
    "// Panics become Flywheel NativeErrors, so the default hook's message is not printed.\n"
    "fn fw_quiet() {\n    static ONCE: std::sync::Once = std::sync::Once::new();\n"
    "    ONCE.call_once(|| std::panic::set_hook(Box::new(|_| {})));\n}\n"
    "fn fw_panic(e: Box<dyn std::any::Any + Send>) {\n"
    "    let m = if let Some(s) = e.downcast_ref::<&str>() { s.to_string() }\n"
    "        else if let Some(s) = e.downcast_ref::<String>() { s.clone() }\n"
    "        else { \"Rust panic\".to_string() };\n"
    "    let c = std::ffi::CString::new(m.replace('\\0', \" \")).unwrap();\n"
    "    unsafe { fw_foreign_fail(c.as_ptr()) }\n}\n";
// --- Zig -----------------------------------------------------------------------------------
std::string zigType(std::vector<std::string> words, bool parameter, std::string &kind) {
    bool errorUnion = !parameter && !words.empty() && words[0] == "!";
    if (errorUnion)
        words.erase(words.begin());
    auto text = join(words, 0, words.size());
    std::string type;
    static const std::set<std::string> integers = {
        "i8",     "i16",     "i32",     "i64",      "u8",         "u16",
        "u32",    "u64",     "isize",   "usize",    "c_int",      "c_uint",
        "c_long", "c_ulong", "c_short", "c_ushort", "c_longlong", "c_ulonglong"};
    if (integers.count(text))
        kind = "int", type = "int";
    else if (text == "f16" || text == "f32" || text == "f64" || text == "c_longdouble")
        kind = "real", type = "double";
    else if (text == "bool")
        kind = "bool", type = "bool";
    else if (text == "[]const u8" || text == "[:0]const u8")
        kind = "slice", type = "string";
    else if (text == "[*:0]const u8")
        kind = "cstr", type = "string";
    else if (text == "void" && !parameter)
        kind = "void", type = "void";
    if (errorUnion && !type.empty())
        kind = "!" + kind;
    return type;
}
void scanZig(const std::string &source, ForeignModule &m) {
    auto t = tokenize(source, "zig");
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "{") {
            i = matching(t, i);
            continue;
        }
        if (t[i] != "fn" || i + 2 >= t.size() || i == 0 ||
            (t[i - 1] != "pub" && t[i - 1] != "export"))
            continue;
        auto name = t[i + 1];
        if (t[i + 2] != "(")
            continue;
        auto close = matching(t, i + 2);
        size_t body = close + 1;
        std::vector<std::string> result;
        for (; body < t.size() && t[body] != "{"; ++body)
            result.push_back(t[body]);
        if (body >= t.size())
            continue;
        ForeignFunction f;
        f.name = name;
        std::string kind;
        f.result = name == "main" ? "" : zigType(result, false, kind);
        f.nativeResult = kind;
        bool ok = !f.result.empty();
        for (auto [a, z] : commaSplit(t, i + 3, close)) {
            size_t colon = a;
            while (colon < z && t[colon] != ":")
                ++colon;
            if (colon == z || t[a] == "comptime") {
                ok = false;
                break;
            }
            std::vector<std::string> words(t.begin() + static_cast<ptrdiff_t>(colon + 1),
                                           t.begin() + static_cast<ptrdiff_t>(z));
            std::string paramKind;
            auto type = zigType(words, true, paramKind);
            if (type.empty()) {
                ok = false;
                break;
            }
            f.params.push_back(type);
            f.nativeParams.push_back(paramKind + "|" + join(words, 0, words.size()));
        }
        if (ok)
            m.functions.push_back(std::move(f));
        i = matching(t, body);
    }
}
std::string zigShim(const ForeignModule &m) {
    std::string s = "const std = @import(\"std\");\nconst user = @import(\"user\");\n"
                    "extern fn fw_foreign_fail(message: [*:0]const u8) void;\n"
                    "extern fn malloc(size: usize) ?[*]u8;\n"
                    "fn fw_out(text: []const u8) ?[*]u8 {\n"
                    "    const p = malloc(text.len + 1) orelse return null;\n"
                    "    @memcpy(p[0..text.len], text);\n    p[text.len] = 0;\n    return p;\n}\n";
    for (auto &f : m.functions) {
        std::string params, args;
        for (size_t i = 0; i < f.params.size(); ++i) {
            auto a = "a" + std::to_string(i);
            auto kind = f.nativeParams[i].substr(0, f.nativeParams[i].find('|'));
            auto abi = f.params[i] == "string"   ? "[*:0]const u8"
                       : f.params[i] == "double" ? "f64"
                                                 : "i64";
            params += (params.empty() ? "" : ", ") + a + ": " + abi;
            std::string arg = kind == "bool"    ? a + " != 0"
                              : kind == "int"   ? "@intCast(" + a + ")"
                              : kind == "real"  ? "@floatCast(" + a + ")"
                              : kind == "slice" ? "std.mem.span(" + a + ")"
                                                : a;
            args += (args.empty() ? "" : ", ") + arg;
        }
        auto kind = f.nativeResult;
        bool errorUnion = !kind.empty() && kind[0] == '!';
        if (errorUnion)
            kind = kind.substr(1);
        auto abiResult = f.result == "string"   ? "?[*]u8"
                         : f.result == "double" ? "f64"
                         : f.result == "void"   ? "void"
                                                : "i64";
        auto failed = f.result == "void"     ? "return;"
                      : f.result == "string" ? "return null;"
                                             : "return 0;";
        std::string call = "user." + f.name + "(" + args + ")";
        if (errorUnion)
            call += " catch |err| {\n        fw_foreign_fail(@errorName(err));\n        " +
                    std::string(failed) + "\n    }";
        std::string body;
        if (f.result == "void")
            body = "    " + call + ";\n";
        else {
            body = "    const r = " + call + ";\n";
            if (kind == "bool")
                body += "    return @as(i64, @intFromBool(r));\n";
            else if (kind == "int")
                body += "    return @intCast(r);\n";
            else if (kind == "real")
                body += "    return @floatCast(r);\n";
            else if (kind == "cstr")
                body += "    return fw_out(std.mem.span(r));\n";
            else
                body += "    return fw_out(r);\n";
        }
        s += "export fn " + f.symbol + "(" + params + ") " + abiResult + " {\n" + body + "}\n";
    }
    return s;
}
// --- Build directives ------------------------------------------------------------------------
// Comments in a C or C++ source name what it needs to compile and link:
//     // flywheel: link SDL3 m          libraries (-lSDL3 -lm, SDL3.lib)
//     // flywheel: pkg sdl3 >= 3.2      a pkg-config package (its --cflags and --libs)
//     // flywheel: cflags -DFAST -Ivendor
//     // flywheel: ldflags -L/opt/lib -Wl,-rpath,/opt/lib
// `// flywheel(linux): ...`, `(windows)`, `(macos)`, and `(unix)` apply on that platform only.
bool directivePlatform(const std::string &platform) {
    if (platform.empty())
        return true;
#ifdef _WIN32
    return platform == "windows";
#elif defined(__APPLE__)
    return platform == "macos" || platform == "unix";
#else
    return platform == "linux" || platform == "unix";
#endif
}
void scanDirectives(const std::string &text, ForeignModule &m) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        auto at = line.find("//");
        if (at == std::string::npos)
            continue;
        size_t p = at + 2;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '/'))
            ++p;
        if (line.compare(p, 8, "flywheel") != 0)
            continue;
        p += 8;
        std::string platform;
        if (p < line.size() && line[p] == '(') {
            auto close = line.find(')', p);
            if (close == std::string::npos)
                continue;
            platform = line.substr(p + 1, close - p - 1);
            p = close + 1;
        }
        if (p >= line.size() || line[p] != ':')
            continue;
        std::istringstream words(line.substr(p + 1));
        std::string verb, word;
        if (!(words >> verb))
            continue;
        if (verb != "link" && verb != "pkg" && verb != "cflags" && verb != "ldflags")
            throw std::runtime_error("unknown directive `flywheel: " + verb + "` in " + m.source +
                                     " (expected link, pkg, cflags, or ldflags)");
        if (platform != "" && platform != "linux" && platform != "windows" &&
            platform != "macos" && platform != "unix")
            throw std::runtime_error("unknown platform `flywheel(" + platform + ")` in " +
                                     m.source + " (expected linux, windows, macos, or unix)");
        if (!directivePlatform(platform))
            continue;
        if (verb == "pkg") {
            // The rest of the line is one pkg-config module, with an optional version test.
            std::string rest;
            std::getline(words, rest);
            auto first = rest.find_first_not_of(" \t");
            auto last = rest.find_last_not_of(" \t\r");
            if (first != std::string::npos)
                m.packages.push_back(rest.substr(first, last - first + 1));
            continue;
        }
        while (words >> word) {
            if (verb == "link")
#ifdef _MSC_VER
                m.linkFlags.push_back(word + ".lib");
#else
                m.linkFlags.push_back("-l" + word);
#endif
            else
                (verb == "cflags" ? m.compileFlags : m.linkFlags).push_back(word);
        }
    }
}
// --- Building --------------------------------------------------------------------------------
int runTool(const std::string &program, const std::vector<std::string> &args, std::string &output);
// The compile and link flags of a module's `flywheel: pkg` packages, from pkg-config.
void packageFlags(const ForeignModule &m, std::vector<std::string> &compile,
                  std::vector<std::string> &link) {
    if (m.packages.empty())
        return;
    auto tool = llvm::sys::findProgramByName("pkg-config");
    if (!tool)
        throw std::runtime_error(m.source + " needs `pkg-config` on PATH (flywheel: pkg " +
                                 m.packages[0] + ")");
    for (auto &package : m.packages) {
        std::vector<std::string> spec;
        std::istringstream words(package);
        for (std::string word; words >> word;)
            spec.push_back(word);
        for (bool libs : {false, true}) {
            std::vector<std::string> args = {libs ? "--libs" : "--cflags"};
            args.insert(args.end(), spec.begin(), spec.end());
            std::string output;
            if (runTool(*tool, args, output) != 0)
                throw std::runtime_error(m.source + " needs the `" + package +
                                         "` development package (flywheel: pkg), which "
                                         "pkg-config cannot find:\n" + output);
            std::istringstream flags(output);
            for (std::string flag; flags >> flag;)
                (libs ? link : compile).push_back(flag);
        }
    }
}
int runTool(const std::string &program, const std::vector<std::string> &args, std::string &output) {
    llvm::SmallString<128> log;
    if (llvm::sys::fs::createTemporaryFile("flywheel-native", "log", log))
        throw std::runtime_error("cannot create a temporary file");
    std::vector<llvm::StringRef> refs{program};
    for (auto &a : args)
        refs.emplace_back(a);
    std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(log),
                                                  llvm::StringRef(log)};
    std::string error;
    int status = llvm::sys::ExecuteAndWait(program, refs, std::nullopt, redirects, 0, 0, &error);
    output = error.empty() ? readText(std::string(log)) : error;
    llvm::sys::fs::remove(log);
    return status;
}
std::string requireTool(const std::vector<std::string> &names, const std::string &language) {
    for (auto &name : names)
        if (auto found = llvm::sys::findProgramByName(name))
            return *found;
    throw std::runtime_error("importing " + language + " needs `" + names[0] + "` on PATH");
}
void compileTool(const std::string &program, const std::vector<std::string> &args,
                 const std::string &language, const std::string &source) {
    std::string output;
    if (runTool(program, args, output) != 0)
        throw std::runtime_error("cannot compile " + language + " source " + source + ":\n" +
                                 output);
}
bool reservedName(const std::string &name) {
    // Flywheel keywords: a function or field named like one could not be called or read.
    static const std::set<std::string> reserved = {
        "import", "as",    "var",      "const",    "class",     "function", "component",  "returns",
        "return", "if",    "else",     "while",     "do",       "for",        "in",
        "end",    "break", "continue", "try",      "catch",     "finally",  "throw",      "inst",
        "attr",   "true",  "false",    "null",     "public",    "private",  "void",       "weak",
        "and",    "or",    "not",      "enum",     "interface", "extends",  "implements", "match",
        "case",   "super", "is",       "elements", "defer"};
    return reserved.count(name) > 0;
}
} // namespace
ForeignModule scanForeign(const std::string &language, const std::string &path) {
    ForeignModule m;
    m.language = language;
    m.source = path;
    auto text = readText(path);
    m.key = contentHash(path).substr(0, 10);
    if (language == "c" || language == "cxx") {
        scanC(text, language == "cxx", m);
        scanDirectives(text, m);
    }
    else if (language == "rust")
        scanRust(text, m);
    else
        scanZig(text, m);
    std::set<std::string> seen;
    std::vector<ForeignFunction> kept;
    for (auto &f : m.functions)
        if (!reservedName(f.name) && seen.insert(f.name).second) {
            f.symbol = "fw_" + language + "_" + m.key + "_" + f.name;
            kept.push_back(std::move(f));
        }
    m.functions = std::move(kept);
    if (m.functions.empty())
        throw std::runtime_error(
            "no importable functions in " + path +
            " (functions need int, floating-point, bool, string, pointer, or void parameters and "
            "results" +
            (language == "rust"  ? ", and `pub`"
             : language == "zig" ? ", and `pub`"
                                 : "") +
            ")");
    if (language == "c" || language == "cxx")
        m.shim = cShim(m, path, language == "cxx");
    else if (language == "rust")
        m.shim = rustShim(m, path);
    else
        m.shim = zigShim(m);
    m.key = contentHash("2|" + text + m.shim).substr(0, 16); // "2": shared libraries use -Bsymbolic
    return m;
}
ForeignBuild buildForeign(const std::vector<ForeignModule> &modules, bool shared) {
    ForeignBuild build;
    if (modules.empty())
        return build;
    auto root = fs::path(cacheDirectory("native"));
#ifdef _WIN32
    std::string sharedSuffix = ".dll", staticSuffix = ".lib", objectSuffix = ".obj";
#else
    std::string sharedSuffix = ".so", staticSuffix = ".a", objectSuffix = ".o";
#endif
    // Rust modules share one crate so the standard library is linked once.
    std::string rustCrate = rustPrelude, rustKey;
    std::vector<const ForeignModule *> rust;
    for (auto &m : modules) {
        if (m.language == "rust") {
            rust.push_back(&m);
            rustCrate += m.shim;
            continue;
        }
        auto directory = root / (m.language + "-" + m.key);
        fs::create_directories(directory);
        auto output =
            directory / ((shared ? "lib" : "shim") + (shared                ? sharedSuffix
                                                      : m.language == "zig" ? staticSuffix
                                                                            : objectSuffix));
        build.inputs.push_back(output.string());
        if (m.language == "zig")
            build.flags.push_back("-lc");
        // Libraries named by `flywheel:` directives: a shared module links them itself; a
        // static build passes them to the program's link.
        std::vector<std::string> compileFlags = m.compileFlags, linkFlags;
        if (!fs::exists(output) || !shared)
            packageFlags(m, compileFlags, linkFlags);
        linkFlags.insert(linkFlags.end(), m.linkFlags.begin(), m.linkFlags.end());
        if (!shared)
            for (auto &flag : linkFlags)
                if (flag.rfind("-l", 0) != 0 ||
                    std::find(build.flags.begin(), build.flags.end(), flag) == build.flags.end())
                    build.flags.push_back(flag);
        if (fs::exists(output))
            continue;
        auto partial = output.string() + ".partial";
        if (m.language == "zig") {
            auto shim = directory / "shim.zig";
            std::ofstream(shim, std::ios::binary) << m.shim;
            auto zig = requireTool({"zig"}, "Zig");
            std::vector<std::string> args = {"build-lib"};
            if (shared)
                args.push_back("-dynamic");
            args.insert(args.end(),
                        {"-O", "ReleaseSafe", "-lc", "-fPIC", "--dep", "user",
                         "-Mroot=" + shim.string(), "-Muser=" + m.source, "-femit-bin=" + partial});
            compileTool(zig, args, "Zig", m.source);
        } else {
            bool cxx = m.language == "cxx";
            auto shim = directory / (cxx ? "shim.cpp" : "shim.c");
            std::ofstream(shim, std::ios::binary) << m.shim;
            std::vector<std::string> args;
            auto rename = "main=fw_foreign_main_" + m.key;
#ifdef _MSC_VER
            args = {"/nologo",     "/O2",
                    "/MD",         "/EHsc",
                    "/c",          cxx ? "/TP" : "/TC",
                    "/D" + rename, "/I" + fs::path(m.source).parent_path().string()};
            args.insert(args.end(), compileFlags.begin(), compileFlags.end());
            args.insert(args.end(), {shim.string(), "/Fo:" + partial});
#else
            args = {"-O2", "-fPIC", "-D" + rename,
                    "-I" + fs::path(m.source).parent_path().string()};
            if (!cxx)
                args.insert(args.begin(), {"-x", "c"});
            args.insert(args.end(), compileFlags.begin(), compileFlags.end());
            args.push_back(shim.string());
            if (shared) {
                args.insert(args.end(), {"-shared", "-o", partial});
#ifndef __APPLE__
                // The module's own functions win over same-named ones already loaded (a C
                // function `step` or `index` would otherwise call the C library's).
                args.push_back("-Wl,-Bsymbolic");
#endif
                // After the source, so the linker resolves its references to them.
                args.insert(args.end(), linkFlags.begin(), linkFlags.end());
            } else
                args.insert(args.end(), {"-c", "-o", partial});
#endif
            compileTool(FW_CXX, args, cxx ? "C++" : "C", m.source);
        }
        fs::rename(partial, output);
    }
    if (!rust.empty()) {
        rustKey = contentHash(rustCrate).substr(0, 16);
        auto directory = root / ("rust-" + rustKey);
        fs::create_directories(directory);
#ifdef _WIN32
        auto library = directory / (shared ? "shim.dll" : "shim.lib");
#else
        auto library = directory / (shared ? "libshim.so" : "libshim.a");
#endif
        auto flagsFile = directory / "link-flags.txt";
        build.inputs.push_back(library.string());
        if (!fs::exists(library)) {
            auto shim = directory / "shim.rs";
            std::ofstream(shim, std::ios::binary) << rustCrate;
            auto rustc = requireTool({"rustc"}, "Rust");
            auto partial = library.string() + ".partial";
            std::vector<std::string> args = {
                "--edition",    "2021",         "--crate-name",
                "fw_shim",      "--crate-type", shared ? "cdylib" : "staticlib",
                "-C",           "opt-level=2",  "-C",
                "panic=unwind", "--print",      "native-static-libs",
                "-o",           partial,        shim.string()};
            std::string output;
            if (runTool(rustc, args, output) != 0) {
                std::string sources;
                for (auto *m : rust)
                    sources += (sources.empty() ? "" : ", ") + m->source;
                throw std::runtime_error("cannot compile Rust source " + sources + ":\n" + output);
            }
            // "note: native-static-libs: -lgcc_s -lutil ..." lists what a staticlib needs.
            std::string flags;
            auto at = output.find("native-static-libs:");
            if (at != std::string::npos) {
                auto end = output.find('\n', at);
                flags = output.substr(at + 19,
                                      end == std::string::npos ? std::string::npos : end - at - 19);
            }
            std::ofstream(flagsFile) << flags;
            fs::rename(partial, library);
        }
        if (!shared) {
            std::ifstream in(flagsFile);
            std::string flag;
            while (in >> flag)
                if (std::find(build.flags.begin(), build.flags.end(), flag) == build.flags.end())
                    build.flags.push_back(flag);
        }
    }
    return build;
}
} // namespace fw
