// CodeTrace — project scanning, background analysis and incremental updates.
#pragma once

#include "core/analysis/analyzer.hpp"
#include "core/analysis/model.hpp"
#include "core/graph/call_graph.hpp"
#include "core/graph/dependency_graph.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace codetrace {

struct ProjectOptions {
    std::string root;
    std::vector<std::string> ignoreDirs = {".git", ".hg", ".svn", "node_modules", "__pycache__",
                                           ".venv", "venv", ".tox", ".mypy_cache", ".idea", ".vs",
                                           ".vscode", "dist", "out", ".cache"};
    bool ignoreBuildDirs = true;               // skips build*/cmake-build-* directories
    std::uint64_t maxFileBytes = 4u << 20;     // larger files are skipped with a diagnostic
    unsigned threads = 0;                      // 0 = hardware concurrency
    bool autoIncludeDirs = true;               // add directories containing C/C++ headers to includeDirs
    AnalyzerOptions analyzer;
};

struct Progress {
    std::size_t done = 0;
    std::size_t total = 0;
    std::string currentFile;
};

using ProgressCallback = std::function<void(const Progress&)>;

/// Cooperative cancellation shared between the UI and the background analysis.
class CancellationToken {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool isCancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }
    void reset() noexcept { cancelled_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

struct LanguageStats {
    Language language = Language::Unknown;
    std::size_t files = 0;
    std::size_t symbols = 0;
};

/// Immutable result of one analysis pass. Safe to share across threads.
struct ProjectSnapshot {
    std::string root;
    std::vector<FileAnalysis> files;           // sorted by path
    std::vector<Symbol> symbols;               // id == index
    CallGraph callGraph;
    DependencyGraph dependencies;
    std::vector<Diagnostic> diagnostics;       // project-level + per file
    std::vector<LanguageStats> languages;
    std::size_t reanalyzedFiles = 0;           // how many files this pass actually parsed
    bool cancelled = false;

    const Symbol* symbol(SymbolId id) const;
    std::vector<SymbolId> findSymbols(std::string_view query, Language lang = Language::Unknown,
                                      bool callablesOnly = false) const;
};

/// Owns the analyzer registry and an incremental per-file cache.
/// analyze() may be called repeatedly: unchanged files are not re-parsed.
class ProjectAnalyzer {
public:
    explicit ProjectAnalyzer(std::shared_ptr<AnalyzerRegistry> registry);

    std::shared_ptr<const ProjectSnapshot> analyze(const ProjectOptions& options,
                                                   const ProgressCallback& progress = {},
                                                   const CancellationToken* cancel = nullptr);

    /// Forget cached results for these files (e.g. reported by a file watcher).
    void invalidate(const std::vector<std::string>& files);
    void clearCache();

    std::shared_ptr<const ProjectSnapshot> lastSnapshot() const;
    const AnalyzerRegistry& registry() const { return *registry_; }

    /// Collect source files under the root, honouring ignore rules.
    std::vector<std::string> discoverFiles(const ProjectOptions& options,
                                           std::vector<Diagnostic>* diagnostics = nullptr) const;

private:
    struct CacheEntry {
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        std::uint64_t hash = 0;
        FileAnalysis analysis;
    };

    FileAnalysis analyzeOne(const SourceFile& file, Language lang, const AnalyzerOptions& options) const;

    std::shared_ptr<AnalyzerRegistry> registry_;
    mutable std::mutex mutex_;
    std::map<std::string, CacheEntry> cache_;
    std::string cacheRoot_;
    std::shared_ptr<const ProjectSnapshot> last_;
};

/// Build symbol table, call graph and dependency graph from per-file results.
std::shared_ptr<ProjectSnapshot> buildSnapshot(std::string root, std::vector<FileAnalysis> files);

std::string normalizePath(std::string_view path);
std::uint64_t fnv1a64(std::string_view data) noexcept;

}  // namespace codetrace
