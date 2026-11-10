// CodeTrace — conversion of the program model to JSON (used by CLI, RPC and extensions).
#pragma once

#include "core/analysis/model.hpp"
#include "core/analysis/project.hpp"
#include "core/protocol/json.hpp"

namespace codetrace::protocol {

json::Value toJson(const SourceRange& r);
json::Value toJson(const Symbol& s);
json::Value toJson(const Diagnostic& d);
json::Value toJson(const CallEdge& e);
json::Value toJson(const FileAnalysis& f);

/// Summary: files, languages, counts, diagnostics.
json::Value summaryJson(const ProjectSnapshot& snap);

/// Full call graph: nodes (callables) + edges + unresolved.
json::Value callGraphJson(const ProjectSnapshot& snap);

/// Neighbourhood of a symbol: callers and callees up to depth.
json::Value neighbourhoodJson(const ProjectSnapshot& snap, SymbolId focus, int depth);

/// File dependency graph + cycles.
json::Value dependencyGraphJson(const ProjectSnapshot& snap);

/// Graphviz DOT of the call graph (for quick visual inspection outside the GUI).
std::string callGraphDot(const ProjectSnapshot& snap);

}  // namespace codetrace::protocol
