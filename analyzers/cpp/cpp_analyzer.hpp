// CodeTrace — C++ analyzers.
#pragma once

#include "core/analysis/analyzer.hpp"

#include <memory>

namespace codetrace::cpp {

/// Dependency-free, error-tolerant structural analyzer. Always available;
/// used directly when libclang is absent and as a fallback when it fails.
class HeuristicCppAnalyzer final : public ILanguageAnalyzer {
public:
    Language language() const noexcept override { return Language::Cpp; }
    std::string_view backendName() const noexcept override { return "heuristic-cpp"; }
    int priority() const noexcept override { return 0; }
    FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions& options) const override;
};

/// libclang-based analyzer (precise, semantic). Returns nullptr when CodeTrace
/// was built without libclang. Disabled at runtime by CODETRACE_CPP_BACKEND=heuristic.
std::shared_ptr<ILanguageAnalyzer> createLibclangAnalyzer();

}  // namespace codetrace::cpp
