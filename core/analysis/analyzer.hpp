// CodeTrace — the single API every language analyzer implements.
#pragma once

#include "core/analysis/model.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace codetrace {

struct SourceFile {
    std::string path;       // normalized absolute path
    std::string content;    // raw bytes (UTF-8 expected, BOM stripped)
};

struct AnalyzerOptions {
    std::vector<std::string> includeDirs;   // extra C/C++ include paths
    std::vector<std::string> defines;       // extra C/C++ defines (NAME or NAME=VALUE)
    std::string projectRoot;
};

/// A language analyzer. Implementations must be thread-safe for concurrent
/// analyze() calls on different files and must never throw for malformed input:
/// problems are reported as diagnostics in the returned FileAnalysis.
class ILanguageAnalyzer {
public:
    virtual ~ILanguageAnalyzer() = default;

    virtual Language language() const noexcept = 0;
    virtual std::string_view backendName() const noexcept = 0;

    /// Higher priority analyzers are preferred when several handle a language.
    virtual int priority() const noexcept { return 0; }

    /// False if a required external component (libclang, python3, ...) is missing.
    virtual bool isAvailable() const noexcept { return true; }

    virtual FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions& options) const = 0;

    /// Files per analyzeBatch() call the project analyzer should use (1 = no batching).
    virtual std::size_t preferredBatchSize() const noexcept { return 1; }

    /// Batch entry point; analyzers with expensive start-up (e.g. an external
    /// process) override this. The default analyzes files one by one.
    virtual std::vector<FileAnalysis> analyzeBatch(std::span<const SourceFile> files,
                                                   const AnalyzerOptions& options) const;
};

/// Holds analyzers and maps file extensions to languages.
/// New languages are added by registering an analyzer and its extensions.
class AnalyzerRegistry {
public:
    void registerAnalyzer(std::shared_ptr<ILanguageAnalyzer> analyzer);
    void registerExtension(std::string extension, Language lang);   // ".cpp" (case-insensitive)

    /// Best available analyzer for the language, or nullptr.
    std::shared_ptr<ILanguageAnalyzer> analyzerFor(Language lang) const;

    /// Next-best analyzer after `current` (used as a fallback when `current` fails).
    std::shared_ptr<ILanguageAnalyzer> fallbackFor(Language lang, const ILanguageAnalyzer* current) const;

    std::vector<std::shared_ptr<ILanguageAnalyzer>> analyzers() const { return analyzers_; }

    /// Detect language by extension, then by shebang for extension-less scripts.
    Language detectLanguage(std::string_view path, std::string_view firstLine = {}) const;

private:
    std::vector<std::shared_ptr<ILanguageAnalyzer>> analyzers_;
    std::vector<std::pair<std::string, Language>> extensions_;
};

}  // namespace codetrace
