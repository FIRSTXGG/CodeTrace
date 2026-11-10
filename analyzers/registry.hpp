// CodeTrace — builds the analyzer registry with every built-in language.
#pragma once

#include "core/analysis/analyzer.hpp"

#include <memory>

namespace codetrace {

/// Registers C++, Python and JavaScript analyzers (precise backends first,
/// heuristic fallbacks always) and their file extensions.
std::shared_ptr<AnalyzerRegistry> createDefaultRegistry();

}  // namespace codetrace
