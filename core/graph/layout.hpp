// CodeTrace — UI-independent layered graph layout (Sugiyama-style, simplified).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace codetrace {

struct LayoutNode {
    double x = 0;
    double y = 0;
    int layer = 0;
    int order = 0;
};

struct LayoutOptions {
    double layerSpacing = 160.0;   // horizontal distance between layers
    double nodeSpacing = 60.0;     // vertical distance between nodes in one layer
    int barycenterPasses = 4;
};

/// Lays out nodes 0..n-1 left-to-right along edge direction. Cycles are tolerated.
/// If `fixedLayers` is non-empty it gives each node's layer (e.g. distance from a focus node).
std::vector<LayoutNode> layeredLayout(std::size_t nodeCount,
                                      const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges,
                                      const LayoutOptions& options = {},
                                      const std::vector<int>& fixedLayers = {});

}  // namespace codetrace
