// CodeTrace — JavaScript analyzers.
#pragma once

#include "core/analysis/analyzer.hpp"

#include <memory>

namespace codetrace::js {

/// Token-based structural analyzer; always available, tolerant of broken code.
class HeuristicJsAnalyzer final : public ILanguageAnalyzer {
public:
    Language language() const noexcept override { return Language::JavaScript; }
    std::string_view backendName() const noexcept override { return "heuristic-js"; }
    int priority() const noexcept override { return 0; }
    FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions& options) const override;
};

/// Tree-sitter based analyzer. nullptr when built without Tree-sitter.
std::shared_ptr<ILanguageAnalyzer> createTreeSitterAnalyzer();

}  // namespace codetrace::js
