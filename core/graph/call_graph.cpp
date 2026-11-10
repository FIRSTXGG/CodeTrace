#include "core/graph/call_graph.hpp"

#include "core/graph/dependency_graph.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

namespace codetrace {

std::string_view toString(Resolution r) noexcept {
    switch (r) {
        case Resolution::Exact: return "exact";
        case Resolution::Likely: return "likely";
        case Resolution::CrossLanguage: return "cross-language";
    }
    return "exact";
}

namespace {

std::string lastComponent(const std::string& name) {
    std::size_t pos = std::string::npos;
    auto colons = name.rfind("::");
    auto dot = name.rfind('.');
    if (colons != std::string::npos) pos = colons + 2;
    if (dot != std::string::npos && (pos == std::string::npos || dot + 1 > pos)) pos = dot + 1;
    return pos == std::string::npos ? name : name.substr(pos);
}

bool endsWithQualified(const std::string& qualified, const std::string& suffix) {
    if (qualified == suffix) return true;
    if (qualified.size() <= suffix.size()) return false;
    if (qualified.compare(qualified.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    char before = qualified[qualified.size() - suffix.size() - 1];
    return before == ':' || before == '.';
}

bool isSelfReceiver(const std::string& r) { return r == "self" || r == "this" || r == "cls"; }

}  // namespace

void CallGraph::build(const std::vector<Symbol>& symbols, const std::vector<FileAnalysis>& files,
                      const DependencyGraph& deps) {
    edges_.clear();
    unresolved_.clear();
    out_.assign(symbols.size(), {});
    in_.assign(symbols.size(), {});

    std::unordered_map<std::string, std::vector<SymbolId>> byName;
    std::map<std::pair<std::string, std::string>, SymbolId> byFileQualified;
    std::unordered_map<std::string, std::vector<SymbolId>> classesByName;
    std::map<std::pair<Language, std::string>, std::vector<SymbolId>> ctorsByClass;
    for (const auto& s : symbols) {
        if (s.kind == SymbolKind::Constructor) ctorsByClass[{s.language, s.container}].push_back(s.id);
        if (isCallable(s.kind)) {
            if (s.kind != SymbolKind::Module) byName[s.name].push_back(s.id);
            byFileQualified.emplace(std::make_pair(s.file, s.qualifiedName), s.id);
        } else if (s.kind == SymbolKind::Class) {
            classesByName[s.name].push_back(s.id);
        }
    }

    // Binding name (as seen from the target language) -> implementation symbols.
    std::map<std::pair<Language, std::string>, std::vector<SymbolId>> bindings;
    for (const auto& f : files) {
        for (const auto& b : f.bindings) {
            auto& list = bindings[{b.toLanguage, b.exportedName}];
            std::string target = lastComponent(b.targetName);
            for (SymbolId id : byName[target])
                if (symbols[id].language == b.fromLanguage) list.push_back(id);
        }
    }

    for (std::uint32_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const auto& f = files[fileIndex];
        const int depIndex = deps.indexOf(f.file);
        std::unordered_set<std::string> importedFiles;
        if (depIndex >= 0)
            for (auto d : deps.dependenciesOf(static_cast<std::uint32_t>(depIndex))) importedFiles.insert(deps.files()[d]);

        for (const auto& call : f.calls) {
            auto callerIt = byFileQualified.find({f.file, call.callerQualifiedName});
            if (callerIt == byFileQualified.end()) continue;
            const SymbolId caller = callerIt->second;
            const Symbol& callerSym = symbols[caller];

            const std::string name = lastComponent(call.calleeName);
            const bool qualified = name.size() != call.calleeName.size();

            std::vector<SymbolId> candidates;
            auto collect = [&](const std::vector<SymbolId>& ids) {
                for (SymbolId id : ids) {
                    const Symbol& s = symbols[id];
                    if (s.language != f.language) continue;
                    if (qualified && !endsWithQualified(s.qualifiedName, call.calleeName)) continue;
                    candidates.push_back(id);
                }
            };
            if (auto it = byName.find(name); it != byName.end()) collect(it->second);

            // `Foo(...)` / `new Foo(...)` -> constructor of class Foo.
            if (candidates.empty()) {
                if (auto it = classesByName.find(name); it != classesByName.end()) {
                    for (SymbolId cls : it->second) {
                        const Symbol& c = symbols[cls];
                        if (c.language != f.language) continue;
                        if (auto ct = ctorsByClass.find({c.language, c.qualifiedName}); ct != ctorsByClass.end())
                            candidates.insert(candidates.end(), ct->second.begin(), ct->second.end());
                    }
                }
            }

            Resolution resolution = Resolution::Exact;
            if (candidates.empty()) {
                if (auto it = bindings.find({f.language, name}); it != bindings.end() && !it->second.empty()) {
                    candidates = it->second;
                    resolution = Resolution::CrossLanguage;
                }
            }

            if (candidates.empty()) {
                unresolved_.push_back({caller, call.calleeName, call.line, 0});
                continue;
            }

            // Rank: 0 same class, 1 same file, 2 imported file, 3 elsewhere.
            const bool memberOfOther = call.isMemberCall && !call.receiver.empty() && !isSelfReceiver(call.receiver);
            auto tier = [&](SymbolId id) {
                const Symbol& s = symbols[id];
                bool sameClass = !callerSym.container.empty() && s.container == callerSym.container &&
                                 (s.kind == SymbolKind::Method || s.kind == SymbolKind::Constructor);
                if (sameClass && !memberOfOther) return 0;
                if (s.file == f.file && !(call.isMemberCall && isSelfReceiver(call.receiver) && !sameClass)) return 1;
                if (importedFiles.count(s.file)) return 2;
                return 3;
            };
            int best = std::numeric_limits<int>::max();
            for (SymbolId id : candidates) best = std::min(best, tier(id));
            std::vector<SymbolId> top;
            for (SymbolId id : candidates)
                if (tier(id) == best) top.push_back(id);

            // Overloads share a qualified name: treat as one target.
            bool sameTarget = std::all_of(top.begin(), top.end(), [&](SymbolId id) {
                return symbols[id].qualifiedName == symbols[top.front()].qualifiedName &&
                       symbols[id].file == symbols[top.front()].file;
            });
            if (!sameTarget) {
                unresolved_.push_back({caller, call.calleeName, call.line, top.size()});
                continue;
            }
            if (resolution != Resolution::CrossLanguage)
                resolution = (best <= 1 && top.size() == 1) ? Resolution::Exact : Resolution::Likely;

            CallEdge e{caller, top.front(), call.line, call.column, resolution};
            out_[caller].push_back(edges_.size());
            in_[e.callee].push_back(edges_.size());
            edges_.push_back(e);
        }
    }
}

std::vector<std::size_t> CallGraph::outgoing(SymbolId id) const { return id < out_.size() ? out_[id] : std::vector<std::size_t>{}; }
std::vector<std::size_t> CallGraph::incoming(SymbolId id) const { return id < in_.size() ? in_[id] : std::vector<std::size_t>{}; }

std::vector<SymbolId> CallGraph::callees(SymbolId id) const {
    std::vector<SymbolId> r;
    for (auto e : outgoing(id))
        if (std::find(r.begin(), r.end(), edges_[e].callee) == r.end()) r.push_back(edges_[e].callee);
    return r;
}

std::vector<SymbolId> CallGraph::callers(SymbolId id) const {
    std::vector<SymbolId> r;
    for (auto e : incoming(id))
        if (std::find(r.begin(), r.end(), edges_[e].caller) == r.end()) r.push_back(edges_[e].caller);
    return r;
}

std::vector<SymbolId> CallGraph::reachable(SymbolId from, int maxDepth, bool forward) const {
    std::vector<SymbolId> result;
    if (from >= out_.size()) return result;
    std::vector<int> depth(out_.size(), -1);
    std::deque<SymbolId> queue{from};
    depth[from] = 0;
    while (!queue.empty()) {
        SymbolId cur = queue.front();
        queue.pop_front();
        if (cur != from) result.push_back(cur);
        if (maxDepth >= 0 && depth[cur] >= maxDepth) continue;
        for (SymbolId next : forward ? callees(cur) : callers(cur)) {
            if (depth[next] >= 0) continue;
            depth[next] = depth[cur] + 1;
            queue.push_back(next);
        }
    }
    return result;
}

std::vector<SymbolId> CallGraph::shortestChain(SymbolId from, SymbolId to) const {
    if (from >= out_.size() || to >= out_.size()) return {};
    if (from == to) return {from};
    std::vector<SymbolId> parent(out_.size(), kInvalidSymbol);
    std::deque<SymbolId> queue{from};
    parent[from] = from;
    while (!queue.empty()) {
        SymbolId cur = queue.front();
        queue.pop_front();
        for (SymbolId next : callees(cur)) {
            if (parent[next] != kInvalidSymbol) continue;
            parent[next] = cur;
            if (next == to) {
                std::vector<SymbolId> chain{to};
                while (chain.back() != from) chain.push_back(parent[chain.back()]);
                std::reverse(chain.begin(), chain.end());
                return chain;
            }
            queue.push_back(next);
        }
    }
    return {};
}

std::vector<std::vector<SymbolId>> CallGraph::recursionCycles() const {
    std::vector<std::vector<std::uint32_t>> adj(out_.size());
    for (SymbolId i = 0; i < out_.size(); ++i) adj[i] = callees(i);
    std::vector<std::vector<SymbolId>> result;
    for (auto& scc : stronglyConnectedComponents(adj)) {
        bool selfLoop = scc.size() == 1 && std::find(adj[scc[0]].begin(), adj[scc[0]].end(), scc[0]) != adj[scc[0]].end();
        if (scc.size() > 1 || selfLoop) {
            std::sort(scc.begin(), scc.end());
            result.push_back(std::move(scc));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::vector<std::uint32_t>> stronglyConnectedComponents(
    const std::vector<std::vector<std::uint32_t>>& adjacency) {
    // Iterative Tarjan: safe for very deep graphs.
    const std::uint32_t n = static_cast<std::uint32_t>(adjacency.size());
    constexpr std::uint32_t kUnvisited = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> index(n, kUnvisited), low(n, 0);
    std::vector<bool> onStack(n, false);
    std::vector<std::uint32_t> stack;
    std::vector<std::vector<std::uint32_t>> components;
    std::uint32_t counter = 0;

    struct Frame {
        std::uint32_t node;
        std::size_t next;
    };
    std::vector<Frame> call;

    for (std::uint32_t start = 0; start < n; ++start) {
        if (index[start] != kUnvisited) continue;
        call.push_back({start, 0});
        index[start] = low[start] = counter++;
        stack.push_back(start);
        onStack[start] = true;

        while (!call.empty()) {
            Frame& fr = call.back();
            const auto& edges = adjacency[fr.node];
            if (fr.next < edges.size()) {
                std::uint32_t w = edges[fr.next++];
                if (w >= n) continue;
                if (index[w] == kUnvisited) {
                    index[w] = low[w] = counter++;
                    stack.push_back(w);
                    onStack[w] = true;
                    call.push_back({w, 0});
                } else if (onStack[w]) {
                    low[fr.node] = std::min(low[fr.node], index[w]);
                }
                continue;
            }
            std::uint32_t v = fr.node;
            call.pop_back();
            if (!call.empty()) low[call.back().node] = std::min(low[call.back().node], low[v]);
            if (low[v] == index[v]) {
                std::vector<std::uint32_t> comp;
                std::uint32_t w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = false;
                    comp.push_back(w);
                } while (w != v);
                components.push_back(std::move(comp));
            }
        }
    }
    return components;
}

}  // namespace codetrace
