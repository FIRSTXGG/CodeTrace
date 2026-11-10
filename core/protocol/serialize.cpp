#include "core/protocol/serialize.hpp"

#include <set>
#include <sstream>

namespace codetrace::protocol {

using json::Value;

Value toJson(const SourceRange& r) {
    Value v = Value::object();
    v["line"] = r.line;
    v["column"] = r.column;
    v["endLine"] = r.endLine;
    v["endColumn"] = r.endColumn;
    return v;
}

Value toJson(const Symbol& s) {
    Value v = Value::object();
    v["id"] = s.id;
    v["name"] = s.name;
    v["qualifiedName"] = s.qualifiedName;
    v["container"] = s.container;
    v["kind"] = toString(s.kind);
    v["language"] = toString(s.language);
    v["file"] = s.file;
    v["range"] = toJson(s.range);
    if (!s.signature.empty()) v["signature"] = s.signature;
    return v;
}

Value toJson(const Diagnostic& d) {
    Value v = Value::object();
    v["severity"] = toString(d.severity);
    v["file"] = d.file;
    v["line"] = d.line;
    v["column"] = d.column;
    v["message"] = d.message;
    v["source"] = d.source;
    return v;
}

Value toJson(const CallEdge& e) {
    Value v = Value::object();
    v["caller"] = e.caller;
    v["callee"] = e.callee;
    v["line"] = e.line;
    v["column"] = e.column;
    v["resolution"] = toString(e.resolution);
    return v;
}

Value toJson(const FileAnalysis& f) {
    Value v = Value::object();
    v["file"] = f.file;
    v["language"] = toString(f.language);
    v["backend"] = f.backend;
    v["symbols"] = f.symbols.size();
    v["calls"] = f.calls.size();
    v["imports"] = f.imports.size();
    v["partial"] = f.partial;
    return v;
}

Value summaryJson(const ProjectSnapshot& snap) {
    Value v = Value::object();
    v["root"] = snap.root;
    v["fileCount"] = snap.files.size();
    v["symbolCount"] = snap.symbols.size();
    v["callCount"] = snap.callGraph.edges().size();
    v["unresolvedCallCount"] = snap.callGraph.unresolved().size();
    v["reanalyzedFiles"] = snap.reanalyzedFiles;
    v["cancelled"] = snap.cancelled;
    Value langs = Value::array();
    for (const auto& l : snap.languages) {
        Value lv = Value::object();
        lv["language"] = toString(l.language);
        lv["files"] = l.files;
        lv["symbols"] = l.symbols;
        langs.push(lv);
    }
    v["languages"] = langs;
    Value files = Value::array();
    for (const auto& f : snap.files) files.push(toJson(f));
    v["files"] = files;
    auto counts = countDiagnostics(snap.diagnostics);
    v["errors"] = counts.errors;
    v["warnings"] = counts.warnings;
    v["dependencyCycles"] = snap.dependencies.cycles().size();
    return v;
}

namespace {

Value graphJson(const ProjectSnapshot& snap, const std::set<SymbolId>& nodes) {
    Value v = Value::object();
    Value ns = Value::array();
    for (SymbolId id : nodes) ns.push(toJson(snap.symbols[id]));
    Value es = Value::array();
    for (const auto& e : snap.callGraph.edges())
        if (nodes.count(e.caller) && nodes.count(e.callee)) es.push(toJson(e));
    v["nodes"] = ns;
    v["edges"] = es;
    return v;
}

}  // namespace

Value callGraphJson(const ProjectSnapshot& snap) {
    std::set<SymbolId> nodes;
    for (const auto& s : snap.symbols)
        if (isCallable(s.kind)) nodes.insert(s.id);
    Value v = graphJson(snap, nodes);
    Value un = Value::array();
    for (const auto& u : snap.callGraph.unresolved()) {
        Value uv = Value::object();
        uv["caller"] = u.caller;
        uv["name"] = u.calleeName;
        uv["line"] = u.line;
        uv["candidates"] = u.candidates;
        un.push(uv);
    }
    v["unresolved"] = un;
    Value cycles = Value::array();
    for (const auto& c : snap.callGraph.recursionCycles()) {
        Value cv = Value::array();
        for (auto id : c) cv.push(id);
        cycles.push(cv);
    }
    v["recursionCycles"] = cycles;
    return v;
}

Value neighbourhoodJson(const ProjectSnapshot& snap, SymbolId focus, int depth) {
    std::set<SymbolId> nodes{focus};
    for (auto id : snap.callGraph.reachable(focus, depth, true)) nodes.insert(id);
    for (auto id : snap.callGraph.reachable(focus, depth, false)) nodes.insert(id);
    Value v = graphJson(snap, nodes);
    v["focus"] = focus;
    return v;
}

Value dependencyGraphJson(const ProjectSnapshot& snap) {
    const auto& g = snap.dependencies;
    Value v = Value::object();
    Value files = Value::array();
    for (const auto& f : g.files()) files.push(f);
    Value edges = Value::array();
    for (const auto& e : g.edges()) {
        Value ev = Value::object();
        ev["from"] = e.from;
        ev["to"] = e.to;
        ev["line"] = e.line;
        ev["spec"] = e.spec;
        edges.push(ev);
    }
    Value ext = Value::array();
    for (const auto& e : g.external()) {
        Value ev = Value::object();
        ev["from"] = e.from;
        ev["module"] = e.module;
        ext.push(ev);
    }
    Value cycles = Value::array();
    for (const auto& c : g.cycles()) {
        Value cv = Value::array();
        for (auto i : c) cv.push(i);
        cycles.push(cv);
    }
    v["files"] = files;
    v["edges"] = edges;
    v["external"] = ext;
    v["cycles"] = cycles;
    return v;
}

std::string callGraphDot(const ProjectSnapshot& snap) {
    std::ostringstream out;
    auto quote = [](const std::string& s) {
        std::string r = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') r.push_back('\\');
            r.push_back(c);
        }
        return r + "\"";
    };
    out << "digraph CodeTrace {\n  rankdir=LR;\n  node [shape=box, fontname=\"Helvetica\"];\n";
    std::set<SymbolId> used;
    for (const auto& e : snap.callGraph.edges()) {
        used.insert(e.caller);
        used.insert(e.callee);
    }
    for (SymbolId id : used) out << "  n" << id << " [label=" << quote(snap.symbols[id].qualifiedName) << "];\n";
    std::set<std::pair<SymbolId, SymbolId>> seen;
    for (const auto& e : snap.callGraph.edges()) {
        if (!seen.insert({e.caller, e.callee}).second) continue;
        out << "  n" << e.caller << " -> n" << e.callee;
        if (e.resolution == Resolution::Likely) out << " [style=dashed]";
        if (e.resolution == Resolution::CrossLanguage) out << " [color=red]";
        out << ";\n";
    }
    out << "}\n";
    return out.str();
}

}  // namespace codetrace::protocol
