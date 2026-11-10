#include "core/graph/layout.hpp"

#include "core/graph/call_graph.hpp"

#include <algorithm>
#include <map>
#include <numeric>

namespace codetrace {

std::vector<LayoutNode> layeredLayout(std::size_t n,
                                      const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges,
                                      const LayoutOptions& options, const std::vector<int>& fixedLayers) {
    std::vector<LayoutNode> nodes(n);
    if (n == 0) return nodes;

    std::vector<std::vector<std::uint32_t>> succ(n), pred(n);
    for (auto [a, b] : edges) {
        if (a >= n || b >= n || a == b) continue;
        succ[a].push_back(b);
        pred[b].push_back(a);
    }

    // 1. Layer assignment.
    std::vector<int> layer(n, 0);
    if (fixedLayers.size() == n) {
        layer = fixedLayers;
    } else {
        // Condense SCCs so cycles do not break longest-path layering.
        auto sccs = stronglyConnectedComponents(succ);
        std::vector<std::uint32_t> comp(n);
        for (std::uint32_t c = 0; c < sccs.size(); ++c)
            for (auto v : sccs[c]) comp[v] = c;
        // Tarjan emits components in reverse topological order.
        std::vector<int> compLayer(sccs.size(), 0);
        for (std::size_t i = sccs.size(); i-- > 0;) {
            for (auto v : sccs[i])
                for (auto w : succ[v])
                    if (comp[w] != i) compLayer[comp[w]] = std::max(compLayer[comp[w]], compLayer[i] + 1);
        }
        for (std::uint32_t v = 0; v < n; ++v) layer[v] = compLayer[comp[v]];
    }
    int minLayer = *std::min_element(layer.begin(), layer.end());
    for (auto& l : layer) l -= minLayer;
    int maxLayer = *std::max_element(layer.begin(), layer.end());

    // 2. Ordering inside layers: barycenter heuristic, a few sweeps.
    std::vector<std::vector<std::uint32_t>> layers(static_cast<std::size_t>(maxLayer) + 1);
    for (std::uint32_t v = 0; v < n; ++v) layers[static_cast<std::size_t>(layer[v])].push_back(v);
    std::vector<double> pos(n, 0);
    for (auto& l : layers)
        for (std::size_t i = 0; i < l.size(); ++i) pos[l[i]] = static_cast<double>(i);

    auto sweep = [&](bool down) {
        auto apply = [&](std::vector<std::uint32_t>& l, const std::vector<std::vector<std::uint32_t>>& neigh) {
            std::vector<std::pair<double, std::uint32_t>> keyed;
            for (auto v : l) {
                const auto& ns = neigh[v];
                double key = pos[v];
                if (!ns.empty()) {
                    double sum = 0;
                    for (auto w : ns) sum += pos[w];
                    key = sum / static_cast<double>(ns.size());
                }
                keyed.emplace_back(key, v);
            }
            std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < keyed.size(); ++i) {
                l[i] = keyed[i].second;
                pos[l[i]] = static_cast<double>(i);
            }
        };
        if (down)
            for (std::size_t i = 1; i < layers.size(); ++i) apply(layers[i], pred);
        else
            for (std::size_t i = layers.size() - 1; i-- > 0;) apply(layers[i], succ);
    };
    for (int p = 0; p < options.barycenterPasses; ++p) sweep(p % 2 == 0);

    // 3. Coordinates: layers left-to-right, nodes centred vertically.
    std::size_t widest = 0;
    for (auto& l : layers) widest = std::max(widest, l.size());
    for (std::size_t li = 0; li < layers.size(); ++li) {
        const auto& l = layers[li];
        double offset = (static_cast<double>(widest) - static_cast<double>(l.size())) * options.nodeSpacing / 2.0;
        for (std::size_t i = 0; i < l.size(); ++i) {
            auto& node = nodes[l[i]];
            node.layer = static_cast<int>(li) + minLayer;
            node.order = static_cast<int>(i);
            node.x = static_cast<double>(li) * options.layerSpacing;
            node.y = offset + static_cast<double>(i) * options.nodeSpacing;
        }
    }
    return nodes;
}

}  // namespace codetrace
