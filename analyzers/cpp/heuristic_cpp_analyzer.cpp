// Structural C++ analysis without a compiler front-end.
//
// The analyzer walks a token stream keeping a stack of scopes (namespace,
// class, function, block, initializer). At every '{' in a declaration context
// the tokens of the current statement are classified to decide what the brace
// opens. Inside function bodies, `identifier (` sequences are recorded as calls.
// It is deliberately forgiving: unbalanced braces, macros and unknown syntax
// degrade results but never abort the analysis.

#include "analyzers/common/lexer.hpp"
#include "analyzers/cpp/cpp_analyzer.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace codetrace::cpp {

using lex::Token;
using lex::TokenKind;

namespace {

const std::unordered_set<std::string_view>& nonCallKeywords() {
    static const std::unordered_set<std::string_view> k = {
        "if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype", "typeid", "static_cast",
        "dynamic_cast", "const_cast", "reinterpret_cast", "catch", "throw", "noexcept", "alignas", "co_await",
        "co_yield", "co_return", "requires", "static_assert", "defined", "do", "else", "case", "default", "new",
        "delete", "operator", "template", "typename", "using", "namespace", "class", "struct", "union", "enum",
        "__attribute__", "__declspec", "_Pragma", "__pragma", "asm", "__asm__", "explicit", "constexpr", "consteval",
        "constinit", "and", "or", "not", "goto", "volatile", "const", "auto", "void", "int", "char", "bool", "float",
        "double", "long", "short", "unsigned", "signed", "this",
    };
    return k;
}

// Identifiers after which `x(` is still an expression (not a declaration `Type x(...)`).
bool exprKeyword(std::string_view s) {
    static constexpr std::array<std::string_view, 12> k = {"return", "else", "case", "co_return", "co_await",
                                                           "throw", "new", "do", "and", "or", "not", "co_yield"};
    return std::find(k.begin(), k.end(), s) != k.end();
}

bool qualifierToken(std::string_view s) {
    static constexpr std::array<std::string_view, 12> k = {"const", "noexcept", "override", "final", "volatile",
                                                           "&", "&&", "mutable", "try", "throw", "constexpr", "->"};
    return std::find(k.begin(), k.end(), s) != k.end();
}

enum class ScopeKind { Global, Namespace, Class, Function, Block, Init, Transparent, Enum };

struct Scope {
    ScopeKind kind;
    std::string qualified;    // qualified name prefix for namespaces/classes, symbol qualified name for functions
    int symbol = -1;          // index into result symbols for functions/classes
};

class Analyzer {
public:
    Analyzer(const SourceFile& file, FileAnalysis& out) : file_(file), out_(out) {}

    void run() {
        toks_ = lex::tokenize(file_.content, lex::Dialect::Cpp);
        scopes_.push_back({ScopeKind::Global, "", -1});
        stmtStart_ = 0;
        for (i_ = 0; i_ < toks_.size(); ++i_) step();
        if (scopes_.size() > 1) {
            unbalanced_ = true;
            std::uint32_t last = toks_.empty() ? 1 : toks_.back().line;
            while (scopes_.size() > 1) closeScope(last);
        }
        if (unbalanced_) {
            out_.partial = true;
            out_.diagnostics.push_back({Severity::Warning, file_.path, unbalancedLine_, 0,
                                        "unbalanced braces; results may be incomplete", "heuristic-cpp"});
        }
    }

private:
    const Token& tok(std::size_t i) const { return toks_[i]; }
    bool inBody() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->kind == ScopeKind::Function) return true;
            if (it->kind != ScopeKind::Block && it->kind != ScopeKind::Init) return false;
        }
        return false;
    }
    const Scope* enclosingFunction() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->kind == ScopeKind::Function) return &*it;
        return nullptr;
    }
    std::string prefix() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->kind == ScopeKind::Namespace || it->kind == ScopeKind::Class || it->kind == ScopeKind::Global)
                return it->qualified;
        return {};
    }
    const Scope* enclosingClass() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->kind == ScopeKind::Class) return &*it;
            if (it->kind == ScopeKind::Namespace || it->kind == ScopeKind::Function) return nullptr;
        }
        return nullptr;
    }
    static std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "::" + b; }

    void step() {
        const Token& t = tok(i_);
        if (t.kind == TokenKind::Preprocessor) {
            directive(t);
            if (!inBody()) stmtStart_ = i_ + 1;
            return;
        }
        if (skipUntilEndif_ > 0) return;

        if (t.kind == TokenKind::Punct) {
            if (t.text == "{") return openBrace();
            if (t.text == "}") return closeBrace();
            if (t.text == ";") {
                if (!inBody()) stmtStart_ = i_ + 1;
                return;
            }
            if (t.text == ":" && !inBody() && i_ > 0) {
                // access specifiers: "public:", "private slots:", "Q_SIGNALS:"
                const auto& p = tok(i_ - 1).text;
                if (p == "public" || p == "private" || p == "protected" || p == "signals" || p == "slots" ||
                    p == "Q_SIGNALS" || p == "Q_SLOTS")
                    stmtStart_ = i_ + 1;
                return;
            }
            return;
        }
        if (t.kind == TokenKind::Identifier && inBody()) maybeCall();
    }

    void directive(const Token& t) {
        std::string_view d = t.text.substr(1);
        while (!d.empty() && (d.front() == ' ' || d.front() == '\t')) d.remove_prefix(1);
        auto word = d.substr(0, d.find_first_of(" \t(<\"\\"));
        if (skipUntilEndif_ > 0) {
            if (word == "if" || word == "ifdef" || word == "ifndef") ++skipUntilEndif_;
            else if (word == "endif") --skipUntilEndif_;
            else if ((word == "else" || word == "elif") && skipUntilEndif_ == 1) skipUntilEndif_ = 0;
            return;
        }
        if (word == "if") {
            auto rest = d.substr(2);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
            if (rest.rfind("0", 0) == 0 && (rest.size() == 1 || rest[1] == ' ' || rest[1] == '\t' || rest[1] == '/'))
                skipUntilEndif_ = 1;
            return;
        }
        if (word == "include" || word == "import" || word == "include_next") {
            auto rest = d.substr(word.size());
            auto q = rest.find_first_of("\"<");
            if (q == std::string_view::npos) return;
            char close = rest[q] == '"' ? '"' : '>';
            auto e = rest.find(close, q + 1);
            if (e == std::string_view::npos) return;
            Import imp;
            imp.module = std::string(rest.substr(q + 1, e - q - 1));
            imp.kind = close == '"' ? ImportKind::Include : ImportKind::SystemInclude;
            imp.line = t.line;
            out_.imports.push_back(std::move(imp));
        }
    }

    void openBrace() {
        if (inBody()) {
            scopes_.push_back({ScopeKind::Block, "", -1});
            return;
        }
        ScopeKind top = scopes_.back().kind;
        if (top == ScopeKind::Init || top == ScopeKind::Enum) {
            scopes_.push_back({ScopeKind::Init, "", -1});
            return;
        }
        classify(stmtStart_, i_);
        if (scopes_.back().kind != ScopeKind::Init) stmtStart_ = i_ + 1;
    }

    void closeBrace() {
        if (scopes_.size() <= 1) {
            if (!unbalanced_) unbalancedLine_ = tok(i_).line;
            unbalanced_ = true;
            stmtStart_ = i_ + 1;
            return;
        }
        ScopeKind k = scopes_.back().kind;
        closeScope(tok(i_).line, tok(i_).column);
        if (k != ScopeKind::Init && !inBody()) stmtStart_ = i_ + 1;
    }

    void closeScope(std::uint32_t line, std::uint32_t col = 0) {
        Scope s = scopes_.back();
        scopes_.pop_back();
        if (s.symbol >= 0) {
            auto& sym = out_.symbols[static_cast<std::size_t>(s.symbol)];
            sym.range.endLine = line;
            sym.range.endColumn = col;
        }
    }

    // Index of matching ')' for '(' at `open` within [open, end), or end.
    std::size_t matchParen(std::size_t open, std::size_t end) const {
        int depth = 0;
        for (std::size_t k = open; k < end; ++k) {
            if (tok(k).isPunct("(")) ++depth;
            else if (tok(k).isPunct(")") && --depth == 0) return k;
        }
        return end;
    }

    std::size_t skipTemplateHeader(std::size_t b, std::size_t e) const {
        while (b < e) {
            if (tok(b).is("template") && b + 1 < e && tok(b + 1).isPunct("<")) {
                int depth = 0;
                std::size_t k = b + 1;
                for (; k < e; ++k) {
                    if (tok(k).isPunct("<")) ++depth;
                    else if (tok(k).isPunct(">") && --depth == 0) break;
                }
                b = k + 1;
                continue;
            }
            if (tok(b).isPunct("[") && b + 1 < e && tok(b + 1).isPunct("[")) {   // [[attr]]
                std::size_t k = b + 2;
                while (k + 1 < e && !(tok(k).isPunct("]") && tok(k + 1).isPunct("]"))) ++k;
                b = k + 2;
                continue;
            }
            if (tok(b).is("export") || tok(b).is("inline") || tok(b).is("static") || tok(b).is("typedef")) {
                ++b;
                continue;
            }
            break;
        }
        return b;
    }

    void classify(std::size_t b, std::size_t e) {
        b = skipTemplateHeader(b, e);
        const std::string pre = prefix();

        if (b >= e) {   // bare block at namespace scope
            scopes_.push_back({ScopeKind::Transparent, "", -1});
            return;
        }

        // extern "C" { ... }
        if (tok(b).is("extern") && b + 1 < e && tok(b + 1).kind == TokenKind::String && b + 2 == e) {
            scopes_.push_back({ScopeKind::Transparent, "", -1});
            return;
        }

        // namespace a::b { ... }
        if (tok(b).is("namespace")) {
            std::string name;
            for (std::size_t k = b + 1; k < e; ++k) {
                if (tok(k).isIdent() && !tok(k).is("inline")) name += std::string(tok(k).text);
                else if (tok(k).isPunct("::")) name += "::";
            }
            if (name.empty()) name = "(anonymous)";
            namespaces_.insert(join(pre, name));
            scopes_.push_back({ScopeKind::Namespace, join(pre, name), -1});
            return;
        }

        // Top-level '(' and '=' positions.
        std::size_t firstParen = e, topAssign = e, lambdaBracket = e;
        {
            int depth = 0;
            for (std::size_t k = b; k < e; ++k) {
                const Token& t = tok(k);
                if (t.kind != TokenKind::Punct) continue;
                if (t.text == "(") {
                    if (depth == 0 && firstParen == e) firstParen = k;
                    ++depth;
                } else if (t.text == ")") {
                    --depth;
                } else if (t.text == "=" && depth == 0 && topAssign == e && !(k > b && tok(k - 1).is("operator"))) {
                    topAssign = k;
                } else if (t.text == "[" && depth == 0 && topAssign != e && lambdaBracket == e && k == topAssign + 1) {
                    lambdaBracket = k;
                }
            }
        }

        // class / struct / union / enum
        std::size_t kw = e;
        for (std::size_t k = b; k < e && k < b + 3; ++k) {
            if (tok(k).is("class") || tok(k).is("struct") || tok(k).is("union") || tok(k).is("enum")) {
                kw = k;
                break;
            }
        }
        if (kw != e && (topAssign == e || topAssign > kw) && (firstParen == e || firstParen > kw || isAttrParen(firstParen))) {
            if (tok(kw).is("enum")) {
                scopes_.push_back({ScopeKind::Enum, "", -1});
                return;
            }
            std::string name;
            int angle = 0;
            for (std::size_t k = kw + 1; k < e; ++k) {
                const Token& t = tok(k);
                if (t.isPunct("<")) { ++angle; continue; }
                if (t.isPunct(">")) { --angle; continue; }
                if (angle > 0) continue;
                if (t.isPunct(":") || t.isPunct("{")) break;
                if (t.isPunct("(")) { k = matchParen(k, e); continue; }   // alignas(..), __declspec(..)
                if (t.isIdent() && !t.is("final") && !t.is("alignas") && !t.is("__declspec") && !t.is("__attribute__"))
                    name = std::string(t.text);
            }
            if (name.empty()) {
                scopes_.push_back({ScopeKind::Class, join(pre, "(anonymous)"), -1});
                return;
            }
            Symbol s;
            s.name = name;
            s.qualifiedName = join(pre, name);
            s.container = pre;
            s.kind = SymbolKind::Class;
            s.language = Language::Cpp;
            s.file = file_.path;
            s.range = {tok(kw).line, tok(kw).column, 0, 0};
            s.signature = std::string(tok(kw).text) + " " + name;
            int idx = static_cast<int>(out_.symbols.size());
            out_.symbols.push_back(std::move(s));
            classNames_.insert(join(pre, name));
            scopes_.push_back({ScopeKind::Class, join(pre, name), idx});
            return;
        }

        // auto f = [](...) { ... }   -> lambda bound to a namespace-scope name
        if (topAssign != e && lambdaBracket != e && topAssign > b && tok(topAssign - 1).isIdent()) {
            std::string name(tok(topAssign - 1).text);
            pushFunction(name, pre, SymbolKind::Lambda, tok(topAssign - 1), b, e);
            return;
        }

        if (topAssign != e || firstParen == e) {
            scopes_.push_back({topAssign != e ? ScopeKind::Init : ScopeKind::Transparent, "", -1});
            return;
        }

        // --- function definition candidate -----------------------------------
        std::size_t paramOpen = firstParen;
        std::size_t nameEnd = firstParen;   // one past the last name token
        std::string name;
        std::size_t nameTok = e;

        // operator overloads
        std::size_t opTok = e;
        for (std::size_t k = b; k < firstParen + 1 && k < e; ++k)
            if (tok(k).is("operator")) { opTok = k; break; }
        if (opTok != e) {
            name = "operator";
            std::size_t k = opTok + 1;
            if (k < e && tok(k).isPunct("(") && k + 1 < e && tok(k + 1).isPunct(")")) {   // operator()
                name += "()";
                k += 2;
            } else {
                for (; k < e && !tok(k).isPunct("("); ++k) {
                    if (tok(k).isIdent()) name += " ";
                    name += std::string(tok(k).text);
                }
            }
            paramOpen = k;
            nameTok = opTok;
            nameEnd = opTok;
            if (paramOpen >= e) {
                scopes_.push_back({ScopeKind::Transparent, "", -1});
                return;
            }
        } else {
            if (firstParen == b || !tok(firstParen - 1).isIdent()) {
                scopes_.push_back({ScopeKind::Transparent, "", -1});
                return;
            }
            nameTok = firstParen - 1;
            name = std::string(tok(nameTok).text);
            if (nonCallKeywords().count(name) && name != "operator") {
                scopes_.push_back({ScopeKind::Transparent, "", -1});
                return;
            }
            if (nameTok > b && tok(nameTok - 1).isPunct("~")) {
                name = "~" + name;
                nameEnd = nameTok - 1;
            } else {
                nameEnd = nameTok;
            }
        }

        // Qualifiers before the name: A::B::name
        std::string qualifier;
        {
            std::size_t k = nameEnd;
            while (k >= b + 2 && tok(k - 1).isPunct("::") && tok(k - 2).isIdent()) {
                qualifier = qualifier.empty() ? std::string(tok(k - 2).text) : std::string(tok(k - 2).text) + "::" + qualifier;
                k -= 2;
            }
            // template args on the qualifier: Foo<T>::bar
            if (qualifier.empty() && k >= b + 2 && tok(k - 1).isPunct("::") && tok(k - 2).isPunct(">")) {
                std::size_t m = k - 2;
                int depth = 0;
                while (m > b) {
                    if (tok(m).isPunct(">")) ++depth;
                    else if (tok(m).isPunct("<") && --depth == 0) break;
                    --m;
                }
                if (m > b && tok(m - 1).isIdent()) qualifier = std::string(tok(m - 1).text);
            }
        }

        std::size_t paramClose = matchParen(paramOpen, e);
        if (paramClose >= e) {
            scopes_.push_back({ScopeKind::Transparent, "", -1});
            return;
        }

        // Tail between ')' and '{': qualifiers, trailing return, ctor init list.
        bool initList = false;
        for (std::size_t k = paramClose + 1; k < e; ++k) {
            const Token& t = tok(k);
            if (t.isPunct(":")) { initList = true; break; }
            if (t.isPunct("(")) { k = matchParen(k, e); continue; }   // noexcept(..), __attribute__((..))
        }
        if (initList) {
            const Token& last = tok(e - 1);
            if (last.isIdent() || last.isPunct(">")) {   // member brace-init:  : m_{x}
                scopes_.push_back({ScopeKind::Init, "", -1});
                return;
            }
        } else if (paramClose + 1 < e) {
            // Reject `MACRO(x) something {` style tails that are not qualifiers.
            bool ok = true;
            for (std::size_t k = paramClose + 1; k < e; ++k) {
                const Token& t = tok(k);
                if (t.isPunct("(")) { k = matchParen(k, e); continue; }
                if (t.isPunct("->")) {   // trailing return type: accept the rest
                    break;
                }
                if (t.isIdent() && (qualifierToken(t.text) || t.text == "requires" || t.text.rfind("__", 0) == 0 ||
                                    t.text == "Q_DECL_OVERRIDE" || t.text == "Q_DECL_NOTHROW"))
                    continue;
                if (t.isPunct("&") || t.isPunct("&&") || t.isPunct("[") || t.isPunct("]")) continue;
                if (t.text == "requires") break;
                ok = false;
                break;
            }
            if (!ok) {
                scopes_.push_back({ScopeKind::Transparent, "", -1});
                return;
            }
        }

        // Test-framework macros: TEST(Suite, Name) { ... }
        if (opTok == e && (name == "TEST" || name == "TEST_F" || name == "TEST_P" || name == "TYPED_TEST" || name == "TEST_CASE")) {
            std::string args;
            for (std::size_t k = paramOpen + 1; k < paramClose; ++k) {
                if (tok(k).isPunct(",")) args += ", ";
                else args += std::string(tok(k).text);
            }
            name += "(" + args + ")";
        }

        const Scope* cls = enclosingClass();
        SymbolKind kind = SymbolKind::Function;
        std::string container = pre;
        if (!qualifier.empty()) {
            container = join(pre, qualifier);
            std::string lastQ = qualifier.substr(qualifier.rfind("::") == std::string::npos ? 0 : qualifier.rfind("::") + 2);
            kind = (name == lastQ) ? SymbolKind::Constructor : SymbolKind::Method;
            // `ns::free_function` where ns is a namespace we have seen.
            if (kind == SymbolKind::Method && namespaces_.count(container)) kind = SymbolKind::Function;
        } else if (cls) {
            std::string clsShort = cls->qualified.substr(cls->qualified.rfind("::") == std::string::npos ? 0 : cls->qualified.rfind("::") + 2);
            kind = (name == clsShort) ? SymbolKind::Constructor : SymbolKind::Method;
        }
        pushFunction(name, container, kind, tok(nameTok), b, paramClose + 1);
    }

    bool isAttrParen(std::size_t paren) const {
        return paren > 0 && (tok(paren - 1).is("alignas") || tok(paren - 1).is("__declspec") || tok(paren - 1).is("__attribute__"));
    }

    void pushFunction(const std::string& name, const std::string& container, SymbolKind kind, const Token& at,
                      std::size_t sigBegin, std::size_t sigEnd) {
        Symbol s;
        s.name = name;
        s.qualifiedName = join(container, name);
        s.container = container;
        s.kind = kind;
        s.language = Language::Cpp;
        s.file = file_.path;
        s.range = {at.line, at.column, 0, 0};
        s.signature = signature(sigBegin, sigEnd);
        int idx = static_cast<int>(out_.symbols.size());
        scopes_.push_back({ScopeKind::Function, s.qualifiedName, idx});
        out_.symbols.push_back(std::move(s));
    }

    std::string signature(std::size_t b, std::size_t e) const {
        std::string out;
        const Token* prev = nullptr;
        for (std::size_t k = b; k < e && out.size() < 240; ++k) {
            const Token& t = tok(k);
            if (t.kind == TokenKind::Preprocessor) continue;
            bool noSpaceBefore = t.isPunct("(") || t.isPunct(")") || t.isPunct(",") || t.isPunct("::") || t.isPunct(">") ||
                                 t.isPunct("*") || t.isPunct("&") || t.isPunct("&&") || t.isPunct("[") || t.isPunct("]");
            bool noSpaceAfterPrev = prev && (prev->isPunct("(") || prev->isPunct("::") || prev->isPunct("<") ||
                                             prev->isPunct("~") || prev->isPunct("["));
            if (!out.empty() && !noSpaceBefore && !noSpaceAfterPrev) out.push_back(' ');
            out += std::string(t.text);
            prev = &t;
        }
        return out;
    }

    void maybeCall() {
        const Token& t = tok(i_);
        std::size_t next = i_ + 1;
        if (next >= toks_.size()) return;
        // Skip explicit template arguments: foo<int, Bar>(...)
        if (tok(next).isPunct("<")) {
            int depth = 0;
            std::size_t k = next;
            for (; k < toks_.size() && k < next + 32; ++k) {
                const Token& u = tok(k);
                if (u.isPunct("<")) ++depth;
                else if (u.isPunct(">") && --depth == 0) break;
                else if (u.isPunct(";") || u.isPunct("{") || u.isPunct("}") || u.isPunct("&&") || u.isPunct("||")) return;
            }
            if (depth != 0 || k + 1 >= toks_.size()) return;
            next = k + 1;
        }
        if (!tok(next).isPunct("(")) return;
        if (nonCallKeywords().count(t.text)) {
            if (t.is("this")) return;
            return;
        }

        CallSite call;
        call.calleeName = std::string(t.text);
        call.line = t.line;
        call.column = t.column;
        if (i_ > 0) {
            const Token& p = tok(i_ - 1);
            if (p.isPunct(".") || p.isPunct("->")) {
                call.isMemberCall = true;
                if (i_ >= 2 && (tok(i_ - 2).isIdent())) call.receiver = std::string(tok(i_ - 2).text);
            } else if (p.isPunct("::")) {
                std::string q = call.calleeName;
                std::size_t k = i_ - 1;
                while (k >= 1 && tok(k).isPunct("::") && tok(k - 1).isIdent()) {
                    q = std::string(tok(k - 1).text) + "::" + q;
                    if (k < 2) break;
                    k -= 2;
                }
                if (q.rfind("std::", 0) == 0) return;   // standard library: never in the project
                call.calleeName = q;
            } else if (p.isPunct("~")) {
                return;   // explicit destructor call
            } else if (p.isIdent() && !exprKeyword(p.text)) {
                return;   // declaration with direct initialisation: `Type name(args)`
            } else if (p.isPunct(">") && next == i_ + 1) {
                return;   // `std::vector<int> v(3)`
            }
        }

        // pybind11: m.def("name", &impl)
        if (call.isMemberCall && call.calleeName == "def") maybeBinding(next);

        const Scope* fn = enclosingFunction();
        if (!fn) return;
        call.callerQualifiedName = fn->qualified;
        out_.calls.push_back(std::move(call));
    }

    void maybeBinding(std::size_t paren) {
        std::size_t k = paren + 1;
        if (k + 3 >= toks_.size() || tok(k).kind != TokenKind::String || !tok(k + 1).isPunct(",")) return;
        std::string exported = lex::unquote(tok(k).text);
        k += 2;
        if (tok(k).isPunct("&")) ++k;
        std::string target;
        while (k < toks_.size() && (tok(k).isIdent() || tok(k).isPunct("::"))) target += std::string(tok(k++).text);
        if (exported.empty() || target.empty()) return;
        out_.bindings.push_back({Language::Cpp, Language::Python, exported, target, tok(paren).line});
    }

    const SourceFile& file_;
    FileAnalysis& out_;
    std::vector<Token> toks_;
    std::vector<Scope> scopes_;
    std::size_t i_ = 0;
    std::size_t stmtStart_ = 0;
    int skipUntilEndif_ = 0;
    bool unbalanced_ = false;
    std::uint32_t unbalancedLine_ = 0;
    std::unordered_set<std::string> classNames_;
    std::unordered_set<std::string> namespaces_;
};

}  // namespace

FileAnalysis HeuristicCppAnalyzer::analyze(const SourceFile& file, const AnalyzerOptions&) const {
    FileAnalysis out;
    out.file = file.path;
    out.language = Language::Cpp;
    out.backend = std::string(backendName());
    Analyzer(file, out).run();
    return out;
}

}  // namespace codetrace::cpp
