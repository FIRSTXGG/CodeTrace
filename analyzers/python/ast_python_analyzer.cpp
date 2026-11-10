// Python analysis through CPython's `ast` module. One helper process handles a
// whole batch of files and prints one JSON object per file. Files that fail to
// parse are re-analyzed with the heuristic analyzer.

#include "analyzers/common/process.hpp"
#include "analyzers/python/python_analyzer.hpp"
#include "core/protocol/json.hpp"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace codetrace::python {

const char* AstPythonAnalyzer::helperScript() {
    return R"PY(
import ast, json, sys
sys.setrecursionlimit(20000)

class Visitor(ast.NodeVisitor):
    def __init__(self):
        self.stack = []          # (kind, qualified)
        self.symbols, self.calls, self.imports = [], [], []

    def prefix(self):
        return self.stack[-1][1] if self.stack else ""

    def caller(self):
        for kind, q in reversed(self.stack):
            if kind == "func":
                return q
        return "<module>"

    def add(self, node, kind, name, signature):
        container = self.prefix()
        qn = container + "." + name if container else name
        self.symbols.append({
            "name": name, "qualifiedName": qn, "container": container, "kind": kind,
            "line": node.lineno, "col": node.col_offset + 1,
            "endLine": getattr(node, "end_lineno", None) or node.lineno,
            "endCol": (getattr(node, "end_col_offset", None) or 0) + 1,
            "signature": signature,
        })
        return qn

    def visit_ClassDef(self, node):
        for d in node.decorator_list: self.visit(d)
        for b in node.bases: self.visit(b)
        for k in node.keywords: self.visit(k)
        qn = self.add(node, "class", node.name, "class " + node.name)
        self.stack.append(("class", qn))
        for s in node.body: self.visit(s)
        self.stack.pop()

    def _func(self, node, is_async):
        for d in node.decorator_list: self.visit(d)
        a = node.args
        for d in list(a.defaults) + [d for d in a.kw_defaults if d is not None]: self.visit(d)
        in_class = bool(self.stack) and self.stack[-1][0] == "class"
        kind = ("constructor" if node.name == "__init__" else "method") if in_class else "function"
        params = [x.arg for x in getattr(a, "posonlyargs", [])] + [x.arg for x in a.args]
        if a.vararg: params.append("*" + a.vararg.arg)
        params += [x.arg for x in a.kwonlyargs]
        if a.kwarg: params.append("**" + a.kwarg.arg)
        sig = ("async def " if is_async else "def ") + node.name + "(" + ", ".join(params) + ")"
        qn = self.add(node, kind, node.name, sig)
        self.stack.append(("func", qn))
        for s in node.body: self.visit(s)
        self.stack.pop()

    def visit_FunctionDef(self, node): self._func(node, False)
    def visit_AsyncFunctionDef(self, node): self._func(node, True)

    def visit_Call(self, node):
        f = node.func
        name, member, receiver = None, False, ""
        if isinstance(f, ast.Name):
            name = f.id
        elif isinstance(f, ast.Attribute):
            name, member = f.attr, True
            if isinstance(f.value, ast.Name):
                receiver = f.value.id
            elif isinstance(f.value, ast.Attribute):
                receiver = f.value.attr
        if name:
            self.calls.append({"caller": self.caller(), "name": name, "receiver": receiver,
                               "member": member, "line": node.lineno, "col": node.col_offset + 1})
        self.generic_visit(node)

    def visit_Import(self, node):
        for a in node.names:
            self.imports.append({"module": a.name, "names": [a.asname] if a.asname else [],
                                 "level": 0, "line": node.lineno})

    def visit_ImportFrom(self, node):
        self.imports.append({"module": node.module or "", "names": [a.name for a in node.names],
                             "level": node.level or 0, "line": node.lineno})

def analyze(path, src):
    try:
        tree = ast.parse(src, filename=path)
    except SyntaxError as e:
        return {"file": path, "ok": False, "error": "SyntaxError: %s" % (e.msg,), "line": e.lineno or 0}
    except Exception as e:
        return {"file": path, "ok": False, "error": "%s: %s" % (type(e).__name__, e), "line": 0}
    v = Visitor()
    try:
        v.visit(tree)
    except RecursionError:
        return {"file": path, "ok": False, "error": "RecursionError: file is nested too deeply", "line": 0}
    return {"file": path, "ok": True, "symbols": v.symbols, "calls": v.calls, "imports": v.imports}

def main():
    # Input: one JSON object per line {"path": ..., "source": ...}; bytes that are
    # not valid UTF-8 survive via surrogateescape and are handed to ast as bytes.
    out = sys.stdout
    with open(sys.argv[1], "r", encoding="utf-8", errors="surrogateescape") as fh:
        for line in fh:
            if not line.strip():
                continue
            try:
                req = json.loads(line)
            except ValueError:
                continue
            src = req.get("source", "").encode("utf-8", "surrogateescape")
            out.write(json.dumps(analyze(req.get("path", ""), src)) + "\n")
    out.flush()

main()
)PY";
}

namespace {

struct HelperScript {
    proc::TempFile file{".py"};
    bool ok = false;
};

HelperScript& helper() {
    static HelperScript h;
    static std::once_flag once;
    std::call_once(once, [] { h.ok = h.file.write(AstPythonAnalyzer::helperScript()); });
    return h;
}

SymbolKind kindFrom(const std::string& k) {
    if (k == "class") return SymbolKind::Class;
    if (k == "method") return SymbolKind::Method;
    if (k == "constructor") return SymbolKind::Constructor;
    return SymbolKind::Function;
}

std::uint32_t u32(const json::Value& v) {
    auto i = v.asInt(0);
    return i < 0 ? 0u : static_cast<std::uint32_t>(i);
}

FileAnalysis fromJson(const json::Value& v, const std::string& path) {
    FileAnalysis fa;
    fa.file = path;
    fa.language = Language::Python;
    fa.backend = "python-ast";
    bool moduleCalls = false;
    for (const auto& s : v["symbols"].asArray()) {
        Symbol sym;
        sym.name = s["name"].asString();
        sym.qualifiedName = s["qualifiedName"].asString();
        sym.container = s["container"].asString();
        sym.kind = kindFrom(s["kind"].asString());
        sym.language = Language::Python;
        sym.file = path;
        sym.range = {u32(s["line"]), u32(s["col"]), u32(s["endLine"]), u32(s["endCol"])};
        sym.signature = s["signature"].asString();
        fa.symbols.push_back(std::move(sym));
    }
    for (const auto& c : v["calls"].asArray()) {
        CallSite cs;
        cs.callerQualifiedName = c["caller"].asString();
        cs.calleeName = c["name"].asString();
        cs.receiver = c["receiver"].asString();
        cs.isMemberCall = c["member"].asBool();
        cs.line = u32(c["line"]);
        cs.column = u32(c["col"]);
        if (cs.callerQualifiedName == "<module>") moduleCalls = true;
        fa.calls.push_back(std::move(cs));
    }
    for (const auto& i : v["imports"].asArray()) {
        Import imp;
        imp.module = i["module"].asString();
        for (const auto& n : i["names"].asArray()) imp.names.push_back(n.asString());
        imp.relativeLevel = static_cast<int>(i["level"].asInt(0));
        imp.kind = imp.relativeLevel ? ImportKind::Relative : ImportKind::Module;
        imp.line = u32(i["line"]);
        fa.imports.push_back(std::move(imp));
    }
    if (moduleCalls) {
        Symbol m;
        m.name = m.qualifiedName = "<module>";
        m.kind = SymbolKind::Module;
        m.language = Language::Python;
        m.file = path;
        m.range = {1, 1, 0, 0};
        fa.symbols.push_back(std::move(m));
    }
    return fa;
}

}  // namespace

AstPythonAnalyzer::AstPythonAnalyzer() {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("CODETRACE_PYTHON"); env && *env) candidates.emplace_back(env);
    if (const char* backend = std::getenv("CODETRACE_PYTHON_BACKEND"); backend && std::string(backend) == "heuristic")
        return;
#ifdef _WIN32
    candidates.insert(candidates.end(), {"py", "python", "python3"});
#else
    candidates.insert(candidates.end(), {"python3", "python"});
#endif
    for (const auto& c : candidates) {
        std::vector<std::string> argv{c};
        if (c == "py") argv.push_back("-3");
        argv.insert(argv.end(), {"-I", "-c", "import ast,sys;print(sys.version_info[0])"});
        auto r = proc::run(argv);
        if (r.started && r.exitCode == 0 && r.output.rfind("3", 0) == 0) {
            interpreter_ = c;
            available_ = true;
            return;
        }
    }
}

bool AstPythonAnalyzer::isAvailable() const noexcept { return available_; }

FileAnalysis AstPythonAnalyzer::analyze(const SourceFile& file, const AnalyzerOptions& options) const {
    auto r = analyzeBatch(std::span<const SourceFile>(&file, 1), options);
    return r.empty() ? fallback_.analyze(file, options) : std::move(r.front());
}

std::vector<FileAnalysis> AstPythonAnalyzer::analyzeBatch(std::span<const SourceFile> files,
                                                          const AnalyzerOptions& options) const {
    std::unordered_map<std::string, json::Value> parsed;
    auto& script = helper();
    if (available_ && script.ok) {
        proc::TempFile list(".txt");
        std::string content;
        for (const auto& f : files) {
            json::Value req = json::Value::object();
            req["path"] = f.path;
            req["source"] = f.content;
            content += req.dump() + "\n";
        }
        if (list.write(content)) {
            std::vector<std::string> argv{interpreter_};
            if (interpreter_ == "py") argv.push_back("-3");
            argv.insert(argv.end(), {"-I", script.file.path(), list.path()});
            auto r = proc::run(argv);
            std::istringstream lines(r.output);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty()) continue;
                try {
                    auto v = json::Value::parse(line);
                    parsed[v["file"].asString()] = std::move(v);
                } catch (const json::ParseError&) {
                    // ignore garbage lines; affected files fall back below
                }
            }
        }
    }

    std::vector<FileAnalysis> out;
    out.reserve(files.size());
    for (const auto& f : files) {
        auto it = parsed.find(f.path);
        if (it != parsed.end() && it->second["ok"].asBool()) {
            out.push_back(fromJson(it->second, f.path));
            continue;
        }
        FileAnalysis fa = fallback_.analyze(f, options);
        if (it != parsed.end()) {
            fa.partial = true;
            fa.diagnostics.push_back({Severity::Warning, f.path, u32(it->second["line"]), 0,
                                      it->second["error"].asString() + " (used heuristic analysis)", "python-ast"});
        } else if (available_) {
            fa.diagnostics.push_back({Severity::Info, f.path, 0, 0, "python helper produced no result; used heuristic analysis",
                                      "python-ast"});
        }
        out.push_back(std::move(fa));
    }
    return out;
}

}  // namespace codetrace::python
