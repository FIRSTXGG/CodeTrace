// CodeTrace — file-level dependency graph built from imports / includes.
#pragma once

#include "core/analysis/model.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace codetrace {

struct FileDependency {
    std::uint32_t from = 0;     // index into files()
    std::uint32_t to = 0;
    std::uint32_t line = 0;     // line of the import in `from`
    std::string spec;           // import text as written
};

struct ExternalDependency {
    std::uint32_t from = 0;
    std::string module;         // unresolved module / header (std lib, third-party)
};

class DependencyGraph {
public:
    void build(const std::vector<FileAnalysis>& files);

    const std::vector<std::string>& files() const { return files_; }
    const std::vector<FileDependency>& edges() const { return edges_; }
    const std::vector<ExternalDependency>& external() const { return external_; }

    int indexOf(const std::string& file) const;   // -1 if unknown
    std::vector<std::uint32_t> dependenciesOf(std::uint32_t file) const;
    std::vector<std::uint32_t> dependentsOf(std::uint32_t file) const;
    bool dependsOn(std::uint32_t from, std::uint32_t to) const;   // direct edge

    /// Cyclic groups of files (each group is a strongly connected component, size > 1 or self-import).
    std::vector<std::vector<std::uint32_t>> cycles() const;

private:
    std::vector<std::string> files_;
    std::unordered_map<std::string, std::uint32_t> index_;
    std::vector<FileDependency> edges_;
    std::vector<ExternalDependency> external_;
    std::vector<std::vector<std::uint32_t>> adjacency_;
};

/// Resolve an import spec to project files (empty if external / unknown).
/// Exposed for testing; uses the same rules as DependencyGraph::build.
class ImportResolver {
public:
    explicit ImportResolver(const std::vector<std::string>& files);
    std::vector<std::string> resolve(const std::string& importingFile, Language lang, const Import& imp) const;

private:
    std::string bySuffix(const std::string& suffix, const std::string& near) const;
    bool exists(const std::string& path) const;

    std::unordered_map<std::string, std::vector<std::string>> byBasename_;
    std::unordered_map<std::string, bool> all_;
};

}  // namespace codetrace
