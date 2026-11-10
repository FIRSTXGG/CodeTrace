// CodeTrace — call graph with name resolution and chain queries.
#pragma once

#include "core/analysis/model.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace codetrace {

class DependencyGraph;

enum class Resolution : std::uint8_t {
    Exact,          // qualified match or same scope
    Likely,         // chosen by import / file proximity heuristics
    CrossLanguage,  // resolved through a LanguageBinding
};

std::string_view toString(Resolution r) noexcept;

struct CallEdge {
    SymbolId caller = kInvalidSymbol;
    SymbolId callee = kInvalidSymbol;
    std::uint32_t line = 0;         // location of the call in the caller's file
    std::uint32_t column = 0;
    Resolution resolution = Resolution::Exact;
};

struct UnresolvedCall {
    SymbolId caller = kInvalidSymbol;
    std::string calleeName;
    std::uint32_t line = 0;
    std::size_t candidates = 0;     // >1 = ambiguous, 0 = external / unknown
};

class CallGraph {
public:
    /// Resolve call sites of all files against the symbol table.
    void build(const std::vector<Symbol>& symbols, const std::vector<FileAnalysis>& files,
               const DependencyGraph& deps);

    const std::vector<CallEdge>& edges() const { return edges_; }
    const std::vector<UnresolvedCall>& unresolved() const { return unresolved_; }

    std::vector<std::size_t> outgoing(SymbolId id) const;   // indices into edges()
    std::vector<std::size_t> incoming(SymbolId id) const;
    std::vector<SymbolId> callees(SymbolId id) const;       // unique
    std::vector<SymbolId> callers(SymbolId id) const;       // unique

    /// All symbols reachable from `from` within `maxDepth` hops (direction: callees or callers).
    std::vector<SymbolId> reachable(SymbolId from, int maxDepth, bool forward) const;

    /// Shortest call chain from -> to, empty if none.
    std::vector<SymbolId> shortestChain(SymbolId from, SymbolId to) const;

    /// Groups of mutually recursive functions (SCCs of size > 1, plus self-loops).
    std::vector<std::vector<SymbolId>> recursionCycles() const;

    std::size_t symbolCount() const { return out_.size(); }

private:
    std::vector<CallEdge> edges_;
    std::vector<UnresolvedCall> unresolved_;
    std::vector<std::vector<std::size_t>> out_;
    std::vector<std::vector<std::size_t>> in_;
};

/// Tarjan's strongly connected components over an adjacency list.
std::vector<std::vector<std::uint32_t>> stronglyConnectedComponents(
    const std::vector<std::vector<std::uint32_t>>& adjacency);

}  // namespace codetrace
