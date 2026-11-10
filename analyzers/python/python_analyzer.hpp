// CodeTrace — Python analyzers.
#pragma once

#include "core/analysis/analyzer.hpp"

#include <memory>
#include <string>

namespace codetrace::python {

/// Indentation-based analyzer; always available, used when python3 is missing
/// and for files the Python parser rejects (syntax errors).
class HeuristicPythonAnalyzer final : public ILanguageAnalyzer {
public:
    Language language() const noexcept override { return Language::Python; }
    std::string_view backendName() const noexcept override { return "heuristic-python"; }
    int priority() const noexcept override { return 0; }
    FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions& options) const override;
};

/// Precise analyzer using CPython's own `ast` module in a helper process.
/// The code being analyzed is only parsed, never executed.
/// Interpreter: $CODETRACE_PYTHON, else python3, else python.
class AstPythonAnalyzer final : public ILanguageAnalyzer {
public:
    AstPythonAnalyzer();
    Language language() const noexcept override { return Language::Python; }
    std::string_view backendName() const noexcept override { return "python-ast"; }
    int priority() const noexcept override { return 10; }
    bool isAvailable() const noexcept override;
    std::size_t preferredBatchSize() const noexcept override { return 48; }

    FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions& options) const override;
    std::vector<FileAnalysis> analyzeBatch(std::span<const SourceFile> files, const AnalyzerOptions& options) const override;

    const std::string& interpreter() const { return interpreter_; }

    /// The embedded helper script (exposed for tests and docs).
    static const char* helperScript();

private:
    std::string interpreter_;
    bool available_ = false;
    HeuristicPythonAnalyzer fallback_;
};

}  // namespace codetrace::python
