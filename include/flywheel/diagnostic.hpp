// Diagnostics shared by the compiler (compile errors, warnings, `flywheel --error`) and the
// runtime (tracebacks of uncaught errors): source excerpts with the failing range marked, color
// on terminals, and the last-error record kept for each source file.
//
//   Flywheel runtime error: NumericError: integer division by zero
//     at main.fw:2:14 in divide
//        2 | return a // b
//          |        ~~^^~~
//     at main.fw:13:7 in <module>
//       13 | print divide(1, 0)
//          |       ^^^^^^^^^^^^
//
// Flywheel Lite (src/diag.c) and flywheel.js (formatDiagnostic) print the same format.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <io.h>
// The console calls color() needs, declared as <windows.h> does so that including this header
// does not bring in (and configure) all of <windows.h> before its includer's own includes.
extern "C" {
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall GetConsoleMode(void *, unsigned long *);
__declspec(dllimport) int __stdcall SetConsoleMode(void *, unsigned long);
}
#else
#include <unistd.h>
#endif

namespace fw::diag {

// A location: line:col is the failing node itself (an operator, a callee, a member name),
// sline:scol up to eline:ecol (exclusive) its whole expression. Columns count bytes from 1;
// eline is 0 when only the position is known.
struct Frame {
    std::string file, function;
    int line = 0, col = 0, sline = 0, scol = 0, eline = 0, ecol = 0;
};
struct Report {
    enum Kind { Runtime, Compile, Warning } kind = Runtime;
    bool bare = false; // runtime: `throw BadInput` without a message prints only the type
    std::string type, message;
    std::vector<Frame> frames; // innermost first
    size_t omitted = 0, omittedAt = 0; // frames left out before frames[omittedAt]
};

inline bool envOn(const char *name) {
    auto v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
// Color on standard error: a terminal, unless NO_COLOR is set; FORCE_COLOR forces it.
inline bool color() {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    if (auto v = std::getenv("NO_COLOR"); v && *v)
        cached = 0;
    else if (envOn("FORCE_COLOR") || envOn("CLICOLOR_FORCE"))
        cached = 1;
    else {
        auto term = std::getenv("TERM");
#ifdef _WIN32
        cached = _isatty(_fileno(stderr)) && !(term && std::strcmp(term, "dumb") == 0);
        if (cached) {
            void *h = GetStdHandle(static_cast<unsigned long>(-12)); // STD_ERROR_HANDLE
            unsigned long mode = 0;
            cached = GetConsoleMode(h, &mode) &&
                     SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
        }
#else
        cached = isatty(fileno(stderr)) && !(term && std::strcmp(term, "dumb") == 0);
#endif
    }
    return cached;
}

namespace detail {
inline const char *const RESET = "\033[0m", *const BOLD = "\033[1m", *const DIM = "\033[2m",
                         *const RED = "\033[1;31m", *const YELLOW = "\033[1;33m",
                         *const MAGENTA = "\033[1;35m", *const CYAN = "\033[36m",
                         *const BLUE = "\033[34m";
inline std::string paint(bool on, const char *code, const std::string &text) {
    return on ? code + text + RESET : text;
}
inline bool wordByte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}
// The end (exclusive, 0-based) of the token at byte `at`: a name or number, a quoted literal,
// or a run of operator characters.
inline size_t tokenEnd(const std::string &s, size_t at) {
    if (at >= s.size())
        return at + 1;
    unsigned char c = static_cast<unsigned char>(s[at]);
    size_t e = at + 1;
    static const char ops[] = "+-*/%=<>!&|^~?:.";
    if (wordByte(c)) {
        while (e < s.size() &&
               (wordByte(static_cast<unsigned char>(s[e])) || (std::isdigit(c) && s[e] == '.')))
            ++e;
    } else if (c == '"' || c == '\'') {
        while (e < s.size() && s[e] != static_cast<char>(c))
            e += s[e] == '\\' ? 2 : 1;
        e = std::min(e + 1, s.size());
    } else if (std::strchr(ops, c)) {
        while (e < s.size() && e < at + 3 && s[e] && std::strchr(ops, s[e]))
            ++e;
    }
    return e;
}
// Display width of bytes [from, to): one column per character, four per tab.
inline int width(const std::string &s, size_t from, size_t to) {
    int w = 0;
    for (size_t i = from; i < to && i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\t')
            w += 4;
        else if ((c & 0xc0) != 0x80)
            ++w;
    }
    return w;
}
inline std::string lineOf(const std::string &text, int line) {
    size_t start = 0;
    for (int l = 1; l < line; ++l) {
        auto nl = text.find('\n', start);
        if (nl == std::string::npos)
            return "";
        start = nl + 1;
    }
    auto nl = text.find('\n', start);
    auto out = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return out;
}
// The source line (indentation removed) and markers under the failing range: `^` under the
// node itself, `~` under the rest of its expression.
inline void excerpt(std::string &out, const Frame &f, int gutter, bool on, const std::string &text,
                    const char *accent) {
    if (f.line < 1)
        return;
    auto s = lineOf(text, f.line);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t n = s.size(), ind = 0;
    while (ind < n && (s[ind] == ' ' || s[ind] == '\t'))
        ++ind;
    if (ind == n)
        return;
    size_t focus = f.col > 0 ? static_cast<size_t>(f.col) - 1 : ind;
    focus = std::min(std::max(focus, ind), n);
    size_t start, end;
    if (f.eline > 0) {
        start = f.sline < f.line                    ? ind
                : f.sline == f.line && f.scol > 0 ? static_cast<size_t>(f.scol) - 1
                                                    : focus;
        end = f.eline > f.line ? n : f.ecol > 0 ? static_cast<size_t>(f.ecol) - 1 : n;
    } else {
        start = focus;
        end = tokenEnd(s, focus);
    }
    start = std::max(start, ind);
    if (focus < start || focus >= end)
        start = std::min(start, focus);
    end = std::min(end, n);
    if (end <= start)
        end = start + 1;
    size_t fend = std::min(tokenEnd(s, focus), end);
    bool whole = focus == start;
    auto shown = [&](size_t from, size_t to) {
        std::string t;
        for (size_t i = from; i < to && i < n; ++i)
            t += s[i] == '\t' ? std::string("    ") : std::string(1, s[i]);
        return t;
    };
    char number[32];
    std::snprintf(number, sizeof number, "%*d |", gutter, f.line);
    out += "    " + paint(on, BLUE, number) + " " + shown(ind, start) + paint(on, BOLD, shown(start, end)) +
           shown(end, n) + "\n";
    std::string marks;
    int before = width(s, start, focus), mid = std::max(1, width(s, focus, fend)),
        after = width(s, fend, end);
    marks.append(static_cast<size_t>(before), whole ? '^' : '~');
    marks.append(static_cast<size_t>(mid), '^');
    marks.append(static_cast<size_t>(after), whole ? '^' : '~');
    out += "    " + paint(on, BLUE, std::string(static_cast<size_t>(gutter), ' ') + " |") + " " +
           std::string(static_cast<size_t>(width(s, ind, start)), ' ') + paint(on, accent, marks) + "\n";
}
inline bool sameFrame(const Frame &a, const Frame &b) {
    return a.file == b.file && a.function == b.function && a.line == b.line && a.col == b.col;
}
} // namespace detail

// A path as shown: relative to the working directory when it lies inside it.
inline std::string shownPath(const std::string &path) {
    std::error_code ec;
    if (path.empty() || path[0] == '<' || !std::filesystem::path(path).is_absolute())
        return path;
    auto cwd = std::filesystem::current_path(ec);
    if (ec)
        return path;
    auto inside = [&](const std::filesystem::path &directory, std::string &out) {
        auto rel = std::filesystem::path(path).lexically_relative(directory);
        if (rel.empty() || rel.generic_string().rfind("..", 0) == 0)
            return false;
        out = rel.make_preferred().string();
        return true;
    };
    // Source paths are canonical; the working directory may not be (a symlink, or a Windows
    // short name such as C:\Users\RUNNER~1), so it is compared canonicalized too.
    std::string out;
    if (inside(cwd, out))
        return out;
    auto canonical = std::filesystem::weakly_canonical(cwd, ec);
    if (!ec && canonical != cwd && inside(canonical, out))
        return out;
    return path;
}

// Reads source files for excerpts; `alias` maps a shown name (<source>) to the file holding it.
struct Sources {
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> alias;
    const std::string *get(const std::string &name) {
        auto path = alias.count(name) ? alias.at(name) : name;
        if (path.empty() || path[0] == '<')
            return nullptr;
        auto found = files.find(path);
        if (found == files.end()) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            std::ostringstream text;
            text << in.rdbuf();
            found = files.emplace(path, text.str()).first;
        }
        return &found->second;
    }
};

// The text of a report. `sources` (when given) adds the source line under each location.
inline std::string render(const Report &r, bool on, Sources *sources) {
    using namespace detail;
    std::string out;
    const char *accent = r.kind == Report::Warning ? YELLOW : RED;
    if (r.kind != Report::Runtime) {
        if (!r.frames.empty()) {
            auto &f = r.frames[0];
            out += paint(on, BOLD, shownPath(f.file) + ":" + std::to_string(f.line) + ":" +
                                       std::to_string(f.col ? f.col : 1) + ":") +
                   " ";
        }
        out += paint(on, accent, r.kind == Report::Warning ? "warning:" : "error:") + " " +
               paint(on, BOLD, r.message) + "\n";
    } else {
        out += paint(on, RED, "Flywheel runtime error:") + " " + paint(on, MAGENTA, r.type);
        if (!r.bare)
            out += ": " + paint(on, BOLD, r.message.empty() ? "exception" : r.message);
        out += "\n";
    }
    int gutter = 1;
    for (auto &f : r.frames)
        gutter = std::max(gutter, static_cast<int>(std::to_string(f.line).size()));
    auto at = [&](const Frame &f) {
        out += "  " + paint(on, DIM, "at") + " " +
               paint(on, CYAN, shownPath(f.file) + ":" + std::to_string(f.line) + ":" +
                                   std::to_string(f.col ? f.col : 1)) +
               " " + paint(on, DIM, "in") + " " + paint(on, BOLD, f.function) + "\n";
    };
    bool covered = false;
    for (size_t i = 0; i < r.frames.size(); ++i) {
        auto &f = r.frames[i];
        if (r.omitted && i == r.omittedAt && !covered)
            out += paint(on, DIM, "  ... " + std::to_string(r.omitted) + " more frame" +
                                      (r.omitted == 1 ? "" : "s") + " ...") +
                   "\n";
        // recursion: the same call three times, then a count
        size_t repeats = 0, hidden = 0;
        while (i + repeats + 1 < r.frames.size() && sameFrame(f, r.frames[i + repeats + 1])) {
            if (r.omitted && i + repeats + 1 == r.omittedAt)
                hidden = r.omitted;
            ++repeats;
        }
        if (r.kind == Report::Runtime)
            at(f);
        if (sources)
            if (auto text = sources->get(f.file))
                excerpt(out, f, gutter, on, *text, accent);
        if (repeats >= 3) {
            at(r.frames[i + 1]);
            at(r.frames[i + 2]);
            auto more = repeats - 2 + hidden;
            out += paint(on, DIM, "  ... the same call " + std::to_string(more) + " more time" +
                                      (more == 1 ? "" : "s") + " ...") +
                   "\n";
            covered = covered || hidden;
            i += repeats;
        }
    }
    return out;
}

// ---------------------------------------------------------------- the last-error record

inline std::string absolute(const std::string &path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
    return ec ? path : canonical.string();
}
// <state dir>/flywheel/errors/<hash of the absolute source path>.txt, or "" when there is no
// state directory: $XDG_STATE_HOME or ~/.local/state, %LOCALAPPDATA% on Windows.
inline std::string recordPath(const std::string &source) {
    auto abs = absolute(source);
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : abs)
        h = (h ^ c) * 1099511628211ULL;
    std::filesystem::path base;
#ifdef _WIN32
    if (auto v = std::getenv("LOCALAPPDATA"); v && *v)
        base = v;
#else
    if (auto v = std::getenv("XDG_STATE_HOME"); v && *v)
        base = v;
    else if (auto home = std::getenv("HOME"); home && *home)
        base = std::filesystem::path(home) / ".local" / "state";
#endif
    if (base.empty())
        return "";
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.txt", static_cast<unsigned long long>(h));
    return (base / "flywheel" / "errors" / name).string();
}
namespace detail {
inline std::string escape(const std::string &s) {
    std::string out;
    for (char c : s)
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c != '\r')
            out += c;
    return out;
}
inline std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else
            out += s[i];
    return out;
}
} // namespace detail
// Keeps the report, without source lines, as the last error of `source` (in the same format
// as Flywheel Lite and flywheel.js). `record` is recordPath(source).
inline void writeRecord(const std::string &record, const std::string &source, const Report &r) {
    using detail::escape;
    if (record.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(record).parent_path(), ec);
    auto tmp = record + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out)
            return;
        out << "flywheel-error 1\nsource " << escape(absolute(source)) << "\ntime "
            << static_cast<long long>(std::time(nullptr)) << "\nkind "
            << (r.kind == Report::Compile ? "compile" : r.bare ? "bare" : "runtime") << "\ntype "
            << escape(r.type) << "\nmessage " << escape(r.message) << "\n";
        if (r.omitted)
            out << "omitted " << r.omittedAt << ' ' << r.omitted << "\n";
        for (auto &f : r.frames)
            out << "frame " << f.line << ' ' << f.col << ' ' << f.sline << ' ' << f.scol << ' '
                << f.eline << ' ' << f.ecol << "\t "
                << escape(f.file.empty() || f.file[0] == '<' ? f.file : absolute(f.file)) << "\nfn "
                << escape(f.function) << "\n";
    }
    std::filesystem::rename(tmp, record, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}
// `flywheel --error FILE`: prints the last error of FILE (locations only); false when none.
inline bool showRecord(const std::string &source) {
    using detail::unescape;
    auto record = recordPath(source);
    std::ifstream in(record, std::ios::binary);
    std::string line;
    if (record.empty() || !in || !std::getline(in, line) || line.rfind("flywheel-error ", 0) != 0)
        return false;
    Report r;
    long long when = 0;
    std::string abs = absolute(source);
    while (std::getline(in, line)) {
        auto sp = line.find(' ');
        auto key = line.substr(0, sp), value = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (key == "time")
            when = std::atoll(value.c_str());
        else if (key == "source")
            abs = unescape(value);
        else if (key == "kind") {
            r.kind = value == "compile" ? Report::Compile : Report::Runtime;
            r.bare = value == "bare";
        } else if (key == "type")
            r.type = unescape(value);
        else if (key == "message")
            r.message = unescape(value);
        else if (key == "omitted")
            std::sscanf(value.c_str(), "%zu %zu", &r.omittedAt, &r.omitted);
        else if (key == "frame") {
            Frame f;
            auto tab = value.find("\t ");
            if (tab == std::string::npos ||
                std::sscanf(value.c_str(), "%d %d %d %d %d %d", &f.line, &f.col, &f.sline, &f.scol,
                            &f.eline, &f.ecol) != 6)
                continue;
            f.file = unescape(value.substr(tab + 2));
            r.frames.push_back(f);
        } else if (key == "fn" && !r.frames.empty())
            r.frames.back().function = unescape(value);
    }
    char stamp[64] = "";
    auto t = static_cast<std::time_t>(when);
    if (auto tm = std::localtime(&t))
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", tm);
    bool on = color();
    std::fflush(stdout);
    auto text = detail::paint(on, detail::DIM, "last error of " + shownPath(abs) + ", " + stamp + ":") +
                "\n" + render(r, on, nullptr);
    std::fwrite(text.data(), 1, text.size(), stderr);
    return true;
}

} // namespace fw::diag
