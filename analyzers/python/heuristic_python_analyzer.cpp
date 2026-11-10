// Indentation-based Python analysis. Source is folded into logical lines
// (bracket continuation, backslashes, triple-quoted strings), strings and
// comments are blanked, then def/class/import statements and `name(` calls are
// recognised per line while a stack of indentation scopes tracks nesting.

#include "analyzers/python/python_analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace codetrace::python {

namespace {

struct LogicalLine {
    std::string text;           // strings replaced by "" / '' placeholders, comments removed
    std::uint32_t line = 0;     // first physical line
    std::uint32_t lastLine = 0;
    int indent = 0;
};

std::vector<LogicalLine> logicalLines(const std::string& src, bool& unterminated) {
    std::vector<LogicalLine> out;
    LogicalLine cur;
    std::uint32_t line = 1;
    int depth = 0;
    bool atLineStart = true;
    int indent = 0;
    std::size_t i = 0;
    unterminated = false;

    auto flush = [&] {
        bool blank = std::all_of(cur.text.begin(), cur.text.end(), [](unsigned char c) { return std::isspace(c); });
        if (!blank) {
            cur.lastLine = line;
            out.push_back(std::move(cur));
        }
        cur = LogicalLine{};
        atLineStart = true;
        indent = 0;
    };

    while (i < src.size()) {
        char c = src[i];
        if (atLineStart) {
            if (c == ' ') { ++indent; ++i; continue; }
            if (c == '\t') { indent = (indent / 8 + 1) * 8; ++i; continue; }
            if (c == '\r' || c == '\f') { ++i; continue; }
            if (c == '\n') { ++line; indent = 0; ++i; continue; }
            if (c == '#') {
                while (i < src.size() && src[i] != '\n') ++i;
                continue;
            }
            atLineStart = false;
            cur.line = line;
            cur.indent = indent;
        }
        if (c == '#') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (c == '\\' && i + 1 < src.size() && (src[i + 1] == '\n' || src[i + 1] == '\r')) {
            i += src[i + 1] == '\r' ? 2 : 1;
            if (i < src.size() && src[i] == '\n') ++i;
            ++line;
            cur.text.push_back(' ');
            continue;
        }
        if (c == '\n') {
            ++line;
            ++i;
            if (depth > 0) {
                cur.text.push_back(' ');
                continue;
            }
            --line;
            flush();
            ++line;
            continue;
        }
        if (c == '"' || c == '\'') {
            bool triple = i + 2 < src.size() && src[i + 1] == c && src[i + 2] == c;
            std::size_t j = i + (triple ? 3 : 1);
            bool closed = false;
            while (j < src.size()) {
                if (src[j] == '\\') { if (j + 1 < src.size() && src[j + 1] == '\n') ++line; j += 2; continue; }
                if (src[j] == '\n') {
                    if (!triple) break;
                    ++line;
                }
                if (src[j] == c && (!triple || (j + 2 < src.size() && src[j + 1] == c && src[j + 2] == c))) {
                    j += triple ? 3 : 1;
                    closed = true;
                    break;
                }
                ++j;
            }
            if (!closed && triple) unterminated = true;
            cur.text.push_back(c);
            cur.text.push_back(c);
            i = std::min(j, src.size());
            continue;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        if ((c == ')' || c == ']' || c == '}') && depth > 0) --depth;
        cur.text.push_back(c);
        ++i;
    }
    flush();
    return out;
}

bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

std::string trim(std::string s) {
    auto b = s.find_first_not_of(" \t");
    auto e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(trim(cur)); cur.clear(); }
        else if (c != '(' && c != ')') cur.push_back(c);
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

bool startsWithWord(const std::string& s, std::string_view w) {
    return s.size() > w.size() && s.compare(0, w.size(), w) == 0 && !isIdentChar(static_cast<unsigned char>(s[w.size()]));
}

const std::unordered_set<std::string_view>& nonCalls() {
    static const std::unordered_set<std::string_view> k = {
        "if", "elif", "while", "for", "return", "and", "or", "not", "in", "is", "def", "class", "lambda", "with",
        "assert", "del", "yield", "await", "except", "raise", "import", "from", "as", "global", "nonlocal", "print_",
        "match", "case",
    };
    return k;
}

struct PyScope {
    int indent;
    bool isClass;
    std::string qualified;
    int symbol;
};

}  // namespace

FileAnalysis HeuristicPythonAnalyzer::analyze(const SourceFile& file, const AnalyzerOptions&) const {
    FileAnalysis out;
    out.file = file.path;
    out.language = Language::Python;
    out.backend = std::string(backendName());

    bool unterminated = false;
    auto lines = logicalLines(file.content, unterminated);
    if (unterminated) {
        out.partial = true;
        out.diagnostics.push_back({Severity::Warning, file.path, 0, 0, "unterminated triple-quoted string", "heuristic-python"});
    }

    std::vector<PyScope> stack;
    bool moduleCalls = false;
    std::uint32_t lastLine = 0;

    auto closeTo = [&](int indent, std::uint32_t endLine) {
        while (!stack.empty() && stack.back().indent >= indent) {
            if (stack.back().symbol >= 0) out.symbols[static_cast<std::size_t>(stack.back().symbol)].range.endLine = endLine;
            stack.pop_back();
        }
    };
    auto qualifiedPrefix = [&] { return stack.empty() ? std::string() : stack.back().qualified; };
    auto callerName = [&]() -> std::string {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (!it->isClass) return it->qualified;
        return "<module>";
    };

    for (const auto& ll : lines) {
        closeTo(ll.indent, lastLine);
        lastLine = ll.lastLine;
        std::string text = trim(ll.text);

        // Decorators belong to the enclosing scope; strip leading '@'.
        if (!text.empty() && text[0] == '@') text = text.substr(1);

        std::string body = text;
        if (startsWithWord(body, "async")) body = trim(body.substr(5));
        const std::size_t bodyOffset = text.size() - body.size();

        const std::string parentCaller = callerName();
        std::size_t scanFrom = 0;            // skip "def name" so it is not taken for a call
        std::size_t headerColon = std::string::npos;
        std::string headerScopeCaller;       // caller for code after the header colon

        if (startsWithWord(body, "def") || startsWithWord(body, "class")) {
            bool isClass = body[0] == 'c';
            std::size_t p = isClass ? 5 : 3;
            while (p < body.size() && body[p] == ' ') ++p;
            std::size_t b = p;
            while (p < body.size() && isIdentChar(static_cast<unsigned char>(body[p]))) ++p;
            std::string name = body.substr(b, p - b);
            if (!name.empty()) {
                int depth = 0;
                for (std::size_t k = p; k < body.size(); ++k) {
                    char c = body[k];
                    if (c == '(' || c == '[' || c == '{') ++depth;
                    else if (c == ')' || c == ']' || c == '}') --depth;
                    else if (c == ':' && depth == 0) { headerColon = bodyOffset + k; break; }
                }
                Symbol s;
                s.name = name;
                s.container = qualifiedPrefix();
                s.qualifiedName = s.container.empty() ? name : s.container + "." + name;
                s.language = Language::Python;
                s.file = file.path;
                s.range = {ll.line, static_cast<std::uint32_t>(ll.indent + 1), ll.lastLine, 0};
                bool inClass = !stack.empty() && stack.back().isClass;
                if (isClass) s.kind = SymbolKind::Class;
                else if (inClass) s.kind = name == "__init__" ? SymbolKind::Constructor : SymbolKind::Method;
                else s.kind = SymbolKind::Function;
                s.signature = trim(headerColon == std::string::npos ? body : text.substr(bodyOffset, headerColon - bodyOffset));
                int idx = static_cast<int>(out.symbols.size());
                std::string qn = s.qualifiedName;
                out.symbols.push_back(std::move(s));
                stack.push_back({ll.indent, isClass, qn, idx});
                scanFrom = bodyOffset + p;
                headerScopeCaller = isClass ? parentCaller : qn;
            }
        } else if (startsWithWord(body, "import")) {
            for (auto& part : splitCommas(body.substr(6))) {
                Import imp;
                auto as = part.find(" as ");
                imp.module = trim(as == std::string::npos ? part : part.substr(0, as));
                if (as != std::string::npos) imp.names.push_back(trim(part.substr(as + 4)));
                imp.kind = ImportKind::Module;
                imp.line = ll.line;
                if (!imp.module.empty()) out.imports.push_back(std::move(imp));
            }
            continue;
        } else if (startsWithWord(body, "from")) {
            auto impPos = body.find(" import ");
            if (impPos != std::string::npos) {
                std::string mod = trim(body.substr(4, impPos - 4));
                Import imp;
                while (!mod.empty() && mod[0] == '.') {
                    ++imp.relativeLevel;
                    mod.erase(0, 1);
                }
                imp.module = mod;
                imp.kind = imp.relativeLevel ? ImportKind::Relative : ImportKind::Module;
                for (auto& n : splitCommas(body.substr(impPos + 8))) {
                    auto as = n.find(" as ");
                    std::string nm = trim(as == std::string::npos ? n : n.substr(0, as));
                    if (!nm.empty()) imp.names.push_back(nm);
                }
                imp.line = ll.line;
                out.imports.push_back(std::move(imp));
            }
            continue;
        }

        // Calls: identifier followed by '(' (whitespace allowed).
        for (std::size_t k = scanFrom; k < text.size(); ++k) {
            unsigned char ch = static_cast<unsigned char>(text[k]);
            if (!isIdentStart(ch) || (k > 0 && isIdentChar(static_cast<unsigned char>(text[k - 1])))) continue;
            std::size_t e = k;
            while (e < text.size() && isIdentChar(static_cast<unsigned char>(text[e]))) ++e;
            std::size_t p = e;
            while (p < text.size() && text[p] == ' ') ++p;
            if (p < text.size() && text[p] == '(') {
                std::string name = text.substr(k, e - k);
                if (!nonCalls().count(name)) {
                    CallSite c;
                    c.calleeName = name;
                    c.line = ll.line;
                    c.column = static_cast<std::uint32_t>(k + 1 + static_cast<std::size_t>(ll.indent));
                    std::size_t d = k;
                    while (d > 0 && text[d - 1] == ' ') --d;
                    if (d > 0 && text[d - 1] == '.') {
                        c.isMemberCall = true;
                        std::size_t r = d - 1, rb = r;
                        while (rb > 0 && isIdentChar(static_cast<unsigned char>(text[rb - 1]))) --rb;
                        c.receiver = text.substr(rb, r - rb);
                    }
                    bool afterHeader = headerColon != std::string::npos && k > headerColon;
                    c.callerQualifiedName = afterHeader ? headerScopeCaller : parentCaller;
                    if (c.callerQualifiedName == "<module>") moduleCalls = true;
                    out.calls.push_back(std::move(c));
                }
            }
            k = e - 1;
        }
    }
    closeTo(-1, lastLine);

    if (moduleCalls) {
        Symbol m;
        m.name = "<module>";
        m.qualifiedName = "<module>";
        m.kind = SymbolKind::Module;
        m.language = Language::Python;
        m.file = file.path;
        m.range = {1, 1, lastLine, 0};
        out.symbols.push_back(std::move(m));
    }
    return out;
}

}  // namespace codetrace::python
