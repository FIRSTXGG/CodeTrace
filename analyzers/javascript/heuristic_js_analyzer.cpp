// Structural JavaScript analysis on a token stream.
//
// Every '{' is classified by looking at the tokens right before it:
//   function f(..) {        -> function f
//   f = (..) => {  f: x => { -> function f
//   name(..) {  (in class / object literal) -> method
//   class A extends B {     -> class A
//   anything else           -> block / object literal
// Expression-bodied named arrows (`const f = x => g(x);`) are tracked until the
// end of their expression. Calls are `identifier (` outside definitions.

#include "analyzers/common/lexer.hpp"
#include "analyzers/javascript/js_analyzer.hpp"

#include <algorithm>
#include <unordered_set>

namespace codetrace::js {

using lex::Token;
using lex::TokenKind;

namespace {

const std::unordered_set<std::string_view>& keywords() {
    static const std::unordered_set<std::string_view> k = {
        "if", "for", "while", "switch", "catch", "with", "return", "typeof", "function", "class", "new", "delete",
        "void", "throw", "await", "yield", "super", "import", "export", "in", "of", "instanceof", "do", "else", "try",
        "finally", "case", "default", "var", "let", "const", "async", "static", "get", "set", "this", "extends",
    };
    return k;
}

bool statementKeyword(std::string_view s) {
    static const std::unordered_set<std::string_view> k = {"const", "let", "var", "function", "class", "export",
                                                          "import", "return", "if", "for", "while", "switch"};
    return k.count(s) != 0;
}

enum class ScopeKind { Module, Function, AnonFunction, Class, Block, ExprFunction };

struct Scope {
    ScopeKind kind;
    std::string qualified;    // qualified name of the function / class / named object
    int symbol = -1;
    int depth = 0;            // bracket depth at creation (ExprFunction)
};

class Analyzer {
public:
    Analyzer(const SourceFile& f, FileAnalysis& out) : file_(f), out_(out) {}

    void run() {
        toks_ = lex::tokenize(file_.content, lex::Dialect::JavaScript);
        scopes_.push_back({ScopeKind::Module, "", -1, 0});
        for (i_ = 0; i_ < toks_.size(); ++i_) step();
        std::uint32_t last = toks_.empty() ? 1 : toks_.back().line;
        if (scopes_.size() > 1) {
            bool onlyExpr = std::all_of(scopes_.begin() + 1, scopes_.end(),
                                        [](const Scope& s) { return s.kind == ScopeKind::ExprFunction; });
            if (!onlyExpr) {
                out_.partial = true;
                out_.diagnostics.push_back({Severity::Warning, file_.path, last, 0,
                                            "unbalanced braces; results may be incomplete", "heuristic-js"});
            }
            while (scopes_.size() > 1) closeScope(last);
        }
        if (moduleCalls_) {
            Symbol m;
            m.name = "<module>";
            m.qualifiedName = "<module>";
            m.kind = SymbolKind::Module;
            m.language = Language::JavaScript;
            m.file = file_.path;
            m.range = {1, 1, last, 0};
            out_.symbols.push_back(std::move(m));
        }
    }

private:
    const Token& tok(std::size_t i) const { return toks_[i]; }
    const Token* at(std::size_t i) const { return i < toks_.size() ? &toks_[i] : nullptr; }

    std::string currentQualified() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (!it->qualified.empty()) return it->qualified;
        return {};
    }
    const Scope* callerScope() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (it->kind == ScopeKind::Function || it->kind == ScopeKind::ExprFunction) return &*it;
        return nullptr;
    }
    std::string enclosingClass() const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->kind == ScopeKind::Class) return it->qualified;
            if (it->kind == ScopeKind::Function) return {};
        }
        return {};
    }
    static std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "." + b; }

    void step() {
        const Token& t = tok(i_);
        closeExpressionFunctions(t);

        if (t.kind == TokenKind::Punct) {
            if (t.text == "(" || t.text == "[") { ++depth_; return; }
            if (t.text == ")" || t.text == "]") { --depth_; return; }
            if (t.text == "{") { openBrace(); ++depth_; return; }
            if (t.text == "}") { --depth_; closeBrace(); return; }
            if (t.text == "=>") { arrow(); return; }
            return;
        }
        if (t.kind == TokenKind::Identifier) {
            if (t.is("import") || t.is("export")) maybeImport();
            else if (t.is("require")) maybeRequire();
            maybeCall();
        }
    }

    void closeExpressionFunctions(const Token& t) {
        while (scopes_.back().kind == ScopeKind::ExprFunction) {
            const Scope& s = scopes_.back();
            bool end = false;
            if (t.kind == TokenKind::Punct) {
                if ((t.text == ")" || t.text == "]" || t.text == "}") && depth_ <= s.depth) end = true;
                if ((t.text == ";" || t.text == ",") && depth_ == s.depth) end = true;
            }
            if (!end && t.isIdent() && statementKeyword(t.text) && depth_ == s.depth && i_ > 0 && tok(i_ - 1).line < t.line)
                end = true;
            if (!end) return;
            closeScope(i_ > 0 ? tok(i_ - 1).line : t.line);
        }
    }

    // Find the name a function value is bound to: `name =`, `name :`, `this.name =`, `export default`.
    // `k` is the index of the first token of the function expression.
    std::string boundName(std::size_t k) const {
        if (k == 0) return {};
        std::size_t p = k - 1;
        if (tok(p).is("async") && p > 0) --p;
        if ((tok(p).isPunct("=") || tok(p).isPunct(":")) && p > 0 && (tok(p - 1).isIdent() || tok(p - 1).kind == TokenKind::String)) {
            if (tok(p).isPunct(":") && p >= 2 && tok(p - 2).isPunct("?")) return {};   // ternary
            std::string n = tok(p - 1).kind == TokenKind::String ? lex::unquote(tok(p - 1).text) : std::string(tok(p - 1).text);
            if (keywords().count(n) && n != "get" && n != "set" && n != "static") return {};
            return n;
        }
        return {};
    }

    std::size_t matchBack(std::size_t close, std::string_view open, std::string_view closeTok) const {
        int d = 0;
        for (std::size_t k = close + 1; k-- > 0;) {
            if (tok(k).isPunct(closeTok)) ++d;
            else if (tok(k).isPunct(open) && --d == 0) return k;
        }
        return close;
    }

    void openBrace() {
        const Token* prev = i_ > 0 ? &tok(i_ - 1) : nullptr;
        if (!prev) return pushBlock({});

        if (prev->isPunct("=>")) {
            std::size_t arrowIdx = i_ - 1;
            std::size_t start = arrowStart(arrowIdx);
            std::string name = boundName(start);
            return name.empty() ? pushAnon() : pushFunction(name, tok(start > 0 ? start - 1 : start), start);
        }

        if (prev->isPunct(")")) {
            std::size_t open = matchBack(i_ - 1, "(", ")");
            if (open == 0) return pushBlock({});
            const Token& before = tok(open - 1);
            if (before.is("function") || (before.isPunct("*") && open >= 2 && tok(open - 2).is("function"))) {
                std::size_t fn = before.is("function") ? open - 1 : open - 2;
                std::string name = boundName(fn);
                return name.empty() ? pushAnon() : pushFunction(name, tok(fn), fn);
            }
            if (before.isIdent()) {
                if (keywords().count(before.text) && !before.is("get") && !before.is("set") && !before.is("static") &&
                    !before.is("async"))
                    return pushBlock({});
                std::size_t nameIdx = open - 1;
                bool declared = nameIdx > 0 && (tok(nameIdx - 1).is("function") ||
                                                (tok(nameIdx - 1).isPunct("*") && nameIdx > 1 && tok(nameIdx - 2).is("function")));
                if (declared) {
                    std::size_t fn = tok(nameIdx - 1).is("function") ? nameIdx - 1 : nameIdx - 2;
                    // `const f = function g() {}` -> bound name wins
                    std::string bound = boundName(fn);
                    return pushFunction(bound.empty() ? std::string(before.text) : bound, before, nameIdx);
                }
                // method shorthand in class body / object literal: name(...) {
                return pushFunction(std::string(before.text), before, nameIdx, true);
            }
            if (before.isPunct("]")) return pushAnon();   // computed method name
            return pushBlock({});
        }

        // class X extends Y {   /   const X = class {
        for (std::size_t k = i_; k-- > 0 && i_ - k <= 12;) {
            const Token& t = tok(k);
            if (t.isPunct(";") || t.isPunct("{") || t.isPunct("}") || t.isPunct("(") || t.isPunct(")")) break;
            if (t.is("class")) {
                std::string name;
                if (k + 1 < i_ && tok(k + 1).isIdent() && !tok(k + 1).is("extends")) name = std::string(tok(k + 1).text);
                if (name.empty()) name = boundName(k);
                if (name.empty()) name = "(anonymous class)";
                Symbol s;
                s.name = name;
                s.container = currentQualified();
                s.qualifiedName = join(s.container, name);
                s.kind = SymbolKind::Class;
                s.language = Language::JavaScript;
                s.file = file_.path;
                s.range = {t.line, t.column, 0, 0};
                s.signature = "class " + name;
                int idx = static_cast<int>(out_.symbols.size());
                scopes_.push_back({ScopeKind::Class, s.qualifiedName, idx, depth_});
                out_.symbols.push_back(std::move(s));
                return;
            }
        }

        // Named object literal: `const api = {`, `api: {`
        if (prev->isPunct("=") || prev->isPunct(":")) {
            std::string name = boundName(i_);
            return pushBlock(name.empty() ? std::string() : join(currentQualified(), name));
        }
        pushBlock({});
    }

    std::size_t arrowStart(std::size_t arrowIdx) const {
        if (arrowIdx == 0) return 0;
        std::size_t p = arrowIdx - 1;
        if (tok(p).isPunct(")")) p = matchBack(p, "(", ")");
        if (p > 0 && tok(p - 1).is("async")) --p;
        return p;
    }

    void arrow() {
        const Token* next = at(i_ + 1);
        if (!next || next->isPunct("{")) return;   // block body handled at '{'
        std::size_t start = arrowStart(i_);
        std::string name = boundName(start);
        if (name.empty()) return;
        removeCallsFrom(start);
        Symbol s = makeFunction(name, tok(start > 0 ? start - 1 : start));
        int idx = static_cast<int>(out_.symbols.size());
        scopes_.push_back({ScopeKind::ExprFunction, s.qualifiedName, idx, depth_});
        out_.symbols.push_back(std::move(s));
    }

    Symbol makeFunction(const std::string& name, const Token& at, bool method = false) {
        std::string cls = enclosingClass();
        Symbol s;
        s.name = name;
        s.container = currentQualified();
        s.qualifiedName = join(s.container, name);
        s.language = Language::JavaScript;
        s.file = file_.path;
        s.range = {at.line, at.column, 0, 0};
        if (!cls.empty() && scopes_.back().kind == ScopeKind::Class) {
            s.kind = name == "constructor" ? SymbolKind::Constructor : SymbolKind::Method;
        } else {
            s.kind = method ? SymbolKind::Method : SymbolKind::Function;
        }
        s.signature = (s.kind == SymbolKind::Function ? "function " : "") + name + "(...)";
        return s;
    }

    void pushFunction(const std::string& name, const Token& at, std::size_t firstTok, bool method = false) {
        removeCallsFrom(firstTok);
        Symbol s = makeFunction(name, at, method);
        int idx = static_cast<int>(out_.symbols.size());
        scopes_.push_back({ScopeKind::Function, s.qualifiedName, idx, depth_});
        out_.symbols.push_back(std::move(s));
    }

    void pushAnon() {
        // Calls inside anonymous callbacks are attributed to the enclosing function.
        scopes_.push_back({ScopeKind::AnonFunction, "", -1, depth_});
    }

    void pushBlock(std::string qualified) { scopes_.push_back({ScopeKind::Block, std::move(qualified), -1, depth_}); }

    void closeBrace() {
        while (scopes_.size() > 1 && scopes_.back().kind == ScopeKind::ExprFunction) closeScope(tok(i_).line);
        if (scopes_.size() <= 1) {
            if (!out_.partial)
                out_.diagnostics.push_back({Severity::Warning, file_.path, tok(i_).line, tok(i_).column,
                                            "unmatched '}'", "heuristic-js"});
            out_.partial = true;
            return;
        }
        closeScope(tok(i_).line, tok(i_).column);
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

    void removeCallsFrom(std::size_t firstTok) {
        while (!callTokens_.empty() && callTokens_.back() >= firstTok) {
            callTokens_.pop_back();
            out_.calls.pop_back();
        }
    }

    void maybeCall() {
        const Token& t = tok(i_);
        const Token* next = at(i_ + 1);
        if (!next) return;
        bool optionalCall = next->isPunct("?.") && at(i_ + 2) && at(i_ + 2)->isPunct("(");
        if (!next->isPunct("(") && !optionalCall) return;
        if (keywords().count(t.text) && !t.is("require")) return;
        const Token* prev = i_ > 0 ? &tok(i_ - 1) : nullptr;
        if (prev && (prev->is("function") || (prev->isPunct("*") && i_ > 1 && tok(i_ - 2).is("function")))) return;

        CallSite call;
        call.calleeName = std::string(t.text);
        call.line = t.line;
        call.column = t.column;
        if (prev && (prev->isPunct(".") || prev->isPunct("?."))) {
            call.isMemberCall = true;
            if (i_ >= 2 && tok(i_ - 2).isIdent()) call.receiver = std::string(tok(i_ - 2).text);
        }
        const Scope* fn = callerScope();
        if (fn) {
            call.callerQualifiedName = fn->qualified;
        } else {
            call.callerQualifiedName = "<module>";
            moduleCalls_ = true;
        }
        out_.calls.push_back(std::move(call));
        callTokens_.push_back(i_);
    }

    void maybeImport() {
        const Token& kw = tok(i_);
        const Token* next = at(i_ + 1);
        if (!next) return;
        if (i_ > 0 && (tok(i_ - 1).isPunct(".") )) return;   // import.meta / obj.import
        if (kw.is("import") && next->isPunct("(")) {           // dynamic import('x')
            const Token* s = at(i_ + 2);
            if (s && s->kind == TokenKind::String) addImport(lex::unquote(s->text), {}, kw.line);
            return;
        }
        if (kw.is("import") && next->kind == TokenKind::String) {   // import 'side-effect'
            addImport(lex::unquote(next->text), {}, kw.line);
            return;
        }
        // import a, { b as c } from 'x'  /  export { a } from 'x'  /  export * from 'x'
        std::vector<std::string> names;
        bool sawFrom = false;
        for (std::size_t k = i_ + 1; k < toks_.size() && k < i_ + 200; ++k) {
            const Token& t = tok(k);
            if (t.isPunct(";")) break;
            if (t.is("from")) { sawFrom = true; continue; }
            if (sawFrom) {
                if (t.kind == TokenKind::String) addImport(lex::unquote(t.text), names, kw.line);
                break;
            }
            if (kw.is("export") && k == i_ + 1 && !t.isPunct("{") && !t.isPunct("*")) break;   // export function ...
            if (t.isIdent() && !t.is("as") && !t.is("type") && !t.is("typeof")) {
                if (k > 0 && tok(k - 1).is("as") && !names.empty()) names.back() = std::string(t.text);
                else names.push_back(std::string(t.text));
            }
            if (t.line > kw.line + 30) break;
        }
    }

    void maybeRequire() {
        const Token* open = at(i_ + 1);
        const Token* s = at(i_ + 2);
        const Token* close = at(i_ + 3);
        if (open && open->isPunct("(") && s && s->kind == TokenKind::String && close && close->isPunct(")")) {
            std::vector<std::string> names;
            // const x = require('m')   /  const { a, b } = require('m')
            if (i_ >= 2 && tok(i_ - 1).isPunct("=") && tok(i_ - 2).isIdent()) names.push_back(std::string(tok(i_ - 2).text));
            addImport(lex::unquote(s->text), names, tok(i_).line);
        }
    }

    void addImport(std::string spec, std::vector<std::string> names, std::uint32_t line) {
        if (spec.empty()) return;
        Import imp;
        imp.kind = spec.rfind('.', 0) == 0 ? ImportKind::Relative : ImportKind::Module;
        imp.module = std::move(spec);
        imp.names = std::move(names);
        imp.line = line;
        out_.imports.push_back(std::move(imp));
    }

    const SourceFile& file_;
    FileAnalysis& out_;
    std::vector<Token> toks_;
    std::vector<Scope> scopes_;
    std::vector<std::size_t> callTokens_;
    std::size_t i_ = 0;
    int depth_ = 0;
    bool moduleCalls_ = false;
};

}  // namespace

FileAnalysis HeuristicJsAnalyzer::analyze(const SourceFile& file, const AnalyzerOptions&) const {
    FileAnalysis out;
    out.file = file.path;
    out.language = Language::JavaScript;
    out.backend = std::string(backendName());
    Analyzer(file, out).run();
    return out;
}

}  // namespace codetrace::js
