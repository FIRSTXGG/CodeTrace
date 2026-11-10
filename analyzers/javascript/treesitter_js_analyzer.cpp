// Precise JavaScript analysis with Tree-sitter (tree-sitter-javascript grammar).
// Compiled only when CODETRACE_HAVE_TREE_SITTER is defined.

#include "analyzers/javascript/js_analyzer.hpp"

#ifndef CODETRACE_HAVE_TREE_SITTER

namespace codetrace::js {
std::shared_ptr<ILanguageAnalyzer> createTreeSitterAnalyzer() { return nullptr; }
}  // namespace codetrace::js

#else

#include "analyzers/common/lexer.hpp"

#include <tree_sitter/api.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" const TSLanguage* tree_sitter_javascript(void);

namespace codetrace::js {

namespace {

class Walker {
public:
    Walker(const std::string& src, FileAnalysis& out) : src_(src), out_(out) {}

    void run(TSNode root) {
        walk(root);
        if (moduleCalls_) {
            Symbol m;
            m.name = m.qualifiedName = "<module>";
            m.kind = SymbolKind::Module;
            m.language = Language::JavaScript;
            m.file = out_.file;
            m.range = {1, 1, ts_node_end_point(root).row + 1, 0};
            out_.symbols.push_back(std::move(m));
        }
    }

private:
    struct Frame {
        std::string qualified;
        bool isClass;
        bool isCallable;
    };

    std::string text(TSNode n) const {
        if (ts_node_is_null(n)) return {};
        auto b = ts_node_start_byte(n), e = ts_node_end_byte(n);
        if (e > src_.size() || b > e) return {};
        return src_.substr(b, e - b);
    }
    static std::string type(TSNode n) { return ts_node_is_null(n) ? std::string() : ts_node_type(n); }
    static TSNode field(TSNode n, const char* name) { return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::strlen(name))); }

    std::string prefix() const { return stack_.empty() ? std::string() : stack_.back().qualified; }
    std::string caller() const {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it)
            if (it->isCallable) return it->qualified;
        return "<module>";
    }
    bool inClassBody() const { return !stack_.empty() && stack_.back().isClass; }

    void addSymbol(TSNode at, TSNode whole, const std::string& name, SymbolKind kind) {
        Symbol s;
        s.name = name;
        s.container = prefix();
        s.qualifiedName = s.container.empty() ? name : s.container + "." + name;
        s.kind = kind;
        s.language = Language::JavaScript;
        s.file = out_.file;
        TSPoint p = ts_node_start_point(at), e = ts_node_end_point(whole);
        s.range = {p.row + 1, p.column + 1, e.row + 1, e.column + 1};
        TSNode params = field(whole, "parameters");
        if (ts_node_is_null(params)) {
            TSNode value = field(whole, "value");
            if (!ts_node_is_null(value)) params = field(value, "parameters");
        }
        s.signature = (kind == SymbolKind::Class ? "class " : "") + name + (kind == SymbolKind::Class ? "" : text(params));
        if (s.signature.size() > 200) s.signature.resize(200);
        out_.symbols.push_back(std::move(s));
    }

    static bool isFunctionValue(const std::string& t) {
        return t == "arrow_function" || t == "function_expression" || t == "function" || t == "generator_function";
    }

    void walkChildren(TSNode n) {
        uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; ++i) walk(ts_node_named_child(n, i));
    }

    void withScope(const std::string& qualified, bool isClass, bool callable, TSNode body) {
        stack_.push_back({qualified, isClass, callable});
        if (!ts_node_is_null(body)) walk(body);
        stack_.pop_back();
    }

    std::string qualify(const std::string& name) const { return prefix().empty() ? name : prefix() + "." + name; }

    void walk(TSNode n) {
        // Bound recursion: worker threads have small stacks and input may be hostile.
        if (depth_ >= kMaxDepth) {
            truncated_ = true;
            return;
        }
        ++depth_;
        walkNode(n);
        --depth_;
    }

    void walkNode(TSNode n) {
        const std::string t = type(n);

        if (t == "function_declaration" || t == "generator_function_declaration") {
            std::string name = text(field(n, "name"));
            addSymbol(field(n, "name"), n, name, SymbolKind::Function);
            return withScope(qualify(name), false, true, field(n, "body"));
        }
        if (t == "class_declaration" || t == "class") {
            std::string name = text(field(n, "name"));
            if (name.empty()) name = pendingName_.empty() ? "(anonymous class)" : pendingName_;
            pendingName_.clear();
            addSymbol(ts_node_is_null(field(n, "name")) ? n : field(n, "name"), n, name, SymbolKind::Class);
            TSNode heritage = ts_node_named_child(n, 1);
            if (type(heritage) == "class_heritage") walk(heritage);
            return withScope(qualify(name), true, false, field(n, "body"));
        }
        if (t == "method_definition") {
            TSNode nameNode = field(n, "name");
            std::string name = type(nameNode) == "computed_property_name" ? "[computed]" : text(nameNode);
            SymbolKind kind = name == "constructor" ? SymbolKind::Constructor : SymbolKind::Method;
            addSymbol(nameNode, n, name, kind);
            return withScope(qualify(name), false, true, field(n, "body"));
        }
        if (t == "variable_declarator" || t == "field_definition" || t == "public_field_definition" || t == "pair" ||
            t == "assignment_expression") {
            TSNode nameNode = t == "pair" ? field(n, "key")
                              : t == "assignment_expression" ? field(n, "left")
                              : t == "variable_declarator" ? field(n, "name") : field(n, "property");
            TSNode value = t == "assignment_expression" ? field(n, "right") : field(n, "value");
            if (type(nameNode) == "member_expression") nameNode = field(nameNode, "property");
            std::string vt = type(value);
            std::string name = text(nameNode);
            if (type(nameNode) == "string") name = lex::unquote(name);
            if (!name.empty() && isFunctionValue(vt)) {
                SymbolKind kind = (inClassBody() || t == "pair") ? SymbolKind::Method : SymbolKind::Function;
                if (t == "assignment_expression") kind = SymbolKind::Function;
                addSymbol(nameNode, value, name, kind);
                return withScope(qualify(name), false, true, field(value, "body"));
            }
            if (!name.empty() && vt == "class") pendingName_ = name;
            if (!name.empty() && vt == "object" && t == "variable_declarator") return withScope(qualify(name), false, false, value);
            if (!name.empty() && t == "variable_declarator" && vt == "call_expression") recordRequire(value, name);
            return walkChildren(n);
        }
        if (isFunctionValue(t)) {   // anonymous callback: calls belong to the enclosing function
            return walk(field(n, "body"));
        }
        if (t == "call_expression") {
            recordCall(n);
            return walkChildren(n);
        }
        if (t == "new_expression") {
            TSNode ctor = field(n, "constructor");
            if (type(ctor) == "identifier") addCall(ctor, text(ctor), "", false);
            return walkChildren(n);
        }
        if (t == "import_statement" || t == "export_statement") {
            TSNode source = field(n, "source");
            if (!ts_node_is_null(source)) {
                Import imp;
                imp.module = lex::unquote(text(source));
                imp.kind = imp.module.rfind('.', 0) == 0 ? ImportKind::Relative : ImportKind::Module;
                imp.line = ts_node_start_point(n).row + 1;
                collectImportNames(n, imp.names);
                if (!imp.module.empty()) out_.imports.push_back(std::move(imp));
                return;
            }
            return walkChildren(n);
        }
        walkChildren(n);
    }

    void collectImportNames(TSNode n, std::vector<std::string>& names) {
        uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode c = ts_node_named_child(n, i);
            std::string ct = type(c);
            if (ct == "string") continue;
            if (ct == "identifier") names.push_back(text(c));
            else if (ct == "import_specifier" || ct == "export_specifier") {
                TSNode alias = field(c, "alias");
                names.push_back(text(ts_node_is_null(alias) ? field(c, "name") : alias));
            } else collectImportNames(c, names);
        }
    }

    void recordRequire(TSNode call, const std::string& boundName) {
        TSNode fn = field(call, "function");
        TSNode args = field(call, "arguments");
        if (text(fn) != "require" || ts_node_named_child_count(args) < 1) return;
        TSNode a = ts_node_named_child(args, 0);
        if (type(a) != "string") return;
        Import imp;
        imp.module = lex::unquote(text(a));
        imp.kind = imp.module.rfind('.', 0) == 0 ? ImportKind::Relative : ImportKind::Module;
        imp.names.push_back(boundName);
        imp.line = ts_node_start_point(call).row + 1;
        out_.imports.push_back(std::move(imp));
        requireHandled_ = ts_node_start_byte(call);
    }

    void recordCall(TSNode n) {
        TSNode fn = field(n, "function");
        std::string ft = type(fn);
        if (ft == "identifier") {
            std::string name = text(fn);
            if (name == "require") {
                if (ts_node_start_byte(n) != requireHandled_) {
                    TSNode args = field(n, "arguments");
                    if (ts_node_named_child_count(args) >= 1 && type(ts_node_named_child(args, 0)) == "string") {
                        Import imp;
                        imp.module = lex::unquote(text(ts_node_named_child(args, 0)));
                        imp.kind = imp.module.rfind('.', 0) == 0 ? ImportKind::Relative : ImportKind::Module;
                        imp.line = ts_node_start_point(n).row + 1;
                        out_.imports.push_back(std::move(imp));
                    }
                }
                return;
            }
            addCall(fn, name, "", false);
        } else if (ft == "member_expression") {
            TSNode obj = field(fn, "object");
            std::string recv = type(obj) == "identifier" || type(obj) == "this" ? text(obj) : std::string();
            addCall(field(fn, "property"), text(field(fn, "property")), recv, true);
        } else if (ft == "import") {
            TSNode args = field(n, "arguments");
            if (ts_node_named_child_count(args) >= 1 && type(ts_node_named_child(args, 0)) == "string") {
                Import imp;
                imp.module = lex::unquote(text(ts_node_named_child(args, 0)));
                imp.kind = imp.module.rfind('.', 0) == 0 ? ImportKind::Relative : ImportKind::Module;
                imp.line = ts_node_start_point(n).row + 1;
                out_.imports.push_back(std::move(imp));
            }
        }
    }

    void addCall(TSNode at, const std::string& name, const std::string& receiver, bool member) {
        if (name.empty()) return;
        CallSite c;
        c.callerQualifiedName = caller();
        c.calleeName = name;
        c.receiver = receiver;
        c.isMemberCall = member;
        TSPoint p = ts_node_start_point(at);
        c.line = p.row + 1;
        c.column = p.column + 1;
        if (c.callerQualifiedName == "<module>") moduleCalls_ = true;
        out_.calls.push_back(std::move(c));
    }

public:
    bool truncated() const { return truncated_; }

private:
    static constexpr int kMaxDepth = 400;
    int depth_ = 0;
    bool truncated_ = false;
    const std::string& src_;
    FileAnalysis& out_;
    std::vector<Frame> stack_;
    std::string pendingName_;
    uint32_t requireHandled_ = UINT32_MAX;
    bool moduleCalls_ = false;
};

void firstError(TSNode n, std::uint32_t& line, std::uint32_t& col) {
    if (ts_node_is_error(n) || ts_node_is_missing(n)) {
        TSPoint p = ts_node_start_point(n);
        line = p.row + 1;
        col = p.column + 1;
        return;
    }
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count && line == 0; ++i) {
        TSNode c = ts_node_child(n, i);
        if (ts_node_has_error(c)) firstError(c, line, col);
    }
}

class TreeSitterJsAnalyzer final : public ILanguageAnalyzer {
public:
    TreeSitterJsAnalyzer() {
        const char* backend = std::getenv("CODETRACE_JS_BACKEND");
        enabled_ = !(backend && std::string(backend) == "heuristic");
    }
    Language language() const noexcept override { return Language::JavaScript; }
    std::string_view backendName() const noexcept override { return "tree-sitter"; }
    int priority() const noexcept override { return 10; }
    bool isAvailable() const noexcept override { return enabled_; }

    FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions&) const override {
        FileAnalysis out;
        out.file = file.path;
        out.language = Language::JavaScript;
        out.backend = "tree-sitter";

        TSParser* parser = ts_parser_new();
        if (!ts_parser_set_language(parser, tree_sitter_javascript())) {
            ts_parser_delete(parser);
            throw std::runtime_error("tree-sitter: incompatible JavaScript grammar version");
        }
        TSTree* tree = ts_parser_parse_string(parser, nullptr, file.content.data(), static_cast<uint32_t>(file.content.size()));
        if (!tree) {
            ts_parser_delete(parser);
            throw std::runtime_error("tree-sitter: parse failed");
        }
        TSNode root = ts_tree_root_node(tree);
        try {
            Walker walker(file.content, out);
            walker.run(root);
            if (walker.truncated()) {
                out.partial = true;
                out.diagnostics.push_back({Severity::Warning, file.path, 0, 0,
                                           "syntax tree nested too deeply; inner code was skipped", "tree-sitter"});
            }
        } catch (...) {
            ts_tree_delete(tree);
            ts_parser_delete(parser);
            throw;
        }
        if (ts_node_has_error(root)) {
            std::uint32_t line = 0, col = 0;
            firstError(root, line, col);
            out.partial = true;
            out.diagnostics.push_back({Severity::Warning, file.path, line, col,
                                       "syntax error; results near this location may be incomplete", "tree-sitter"});
        }
        ts_tree_delete(tree);
        ts_parser_delete(parser);
        return out;
    }

private:
    bool enabled_ = true;
};

}  // namespace

std::shared_ptr<ILanguageAnalyzer> createTreeSitterAnalyzer() { return std::make_shared<TreeSitterJsAnalyzer>(); }

}  // namespace codetrace::js

#endif
