#include "core/graph/dependency_graph.hpp"

#include "core/analysis/project.hpp"
#include "core/graph/call_graph.hpp"

#include <algorithm>
#include <set>

namespace codetrace {

namespace {

std::string dirOf(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string baseOf(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::size_t commonPrefix(const std::string& a, const std::string& b) {
    std::size_t n = std::min(a.size(), b.size()), i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

std::string join(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return normalizePath(rel);
    return normalizePath(dir + "/" + rel);
}

}  // namespace

ImportResolver::ImportResolver(const std::vector<std::string>& files) {
    for (const auto& f : files) {
        byBasename_[baseOf(f)].push_back(f);
        all_[f] = true;
    }
}

bool ImportResolver::exists(const std::string& path) const { return all_.count(path) != 0; }

std::string ImportResolver::bySuffix(const std::string& suffix, const std::string& near) const {
    // suffix is a relative path like "foo/bar.h"; it must match at a '/' boundary.
    auto it = byBasename_.find(baseOf(suffix));
    if (it == byBasename_.end()) return {};
    const std::string needle = "/" + suffix;
    std::string best;
    std::size_t bestScore = 0;
    for (const auto& candidate : it->second) {
        bool match = candidate == suffix ||
                     (candidate.size() > needle.size() &&
                      candidate.compare(candidate.size() - needle.size(), needle.size(), needle) == 0);
        if (!match) continue;
        std::size_t score = commonPrefix(candidate, near) + 1;
        if (score > bestScore || (score == bestScore && candidate < best)) {
            best = candidate;
            bestScore = score;
        }
    }
    return best;
}

std::vector<std::string> ImportResolver::resolve(const std::string& importingFile, Language lang,
                                                 const Import& imp) const {
    std::vector<std::string> out;
    const std::string dir = dirOf(importingFile);
    auto add = [&](const std::string& p) {
        if (!p.empty() && p != importingFile && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };

    switch (lang) {
        case Language::Cpp: {
            if (imp.module.empty()) break;
            if (imp.kind == ImportKind::Include) {
                std::string local = join(dir, imp.module);
                if (exists(local)) {
                    add(local);
                    break;
                }
            }
            add(bySuffix(normalizePath(imp.module), importingFile));
            break;
        }
        case Language::Python: {
            std::string modPath = imp.module;
            std::replace(modPath.begin(), modPath.end(), '.', '/');
            auto tryModule = [&](const std::string& base, const std::string& rel) -> std::string {
                if (rel.empty()) return {};
                if (!base.empty() || imp.relativeLevel > 0) {
                    for (const char* suffix : {".py", "/__init__.py"}) {
                        std::string p = join(base, rel + suffix);
                        if (exists(p)) return p;
                    }
                    return {};
                }
                for (const char* suffix : {".py", "/__init__.py"}) {
                    std::string p = bySuffix(rel + suffix, importingFile);
                    if (!p.empty()) return p;
                }
                return {};
            };
            std::string base;
            if (imp.relativeLevel > 0) {
                base = dir;
                for (int i = 1; i < imp.relativeLevel; ++i) base = dirOf(base);
            }
            std::string resolved = tryModule(base, modPath);
            add(resolved);
            // "from pkg import sub" may import submodules.
            for (const auto& name : imp.names) {
                if (name == "*") continue;
                std::string rel = modPath.empty() ? name : modPath + "/" + name;
                add(tryModule(base, rel));
            }
            if (imp.relativeLevel > 0 && modPath.empty() && out.empty()) {
                std::string init = join(base, "__init__.py");
                if (exists(init)) add(init);
            }
            break;
        }
        case Language::JavaScript: {
            const std::string& spec = imp.module;
            bool relative = spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0 || spec.rfind('/', 0) == 0;
            if (!relative) break;   // bare specifier: npm package
            std::string base = spec.front() == '/' ? normalizePath(spec) : join(dir, spec);
            for (const char* ext : {"", ".js", ".mjs", ".cjs", ".jsx", "/index.js", "/index.mjs", "/index.jsx"}) {
                std::string p = base + ext;
                if (exists(p)) {
                    add(p);
                    break;
                }
            }
            break;
        }
        case Language::Unknown: break;
    }
    return out;
}

void DependencyGraph::build(const std::vector<FileAnalysis>& files) {
    files_.clear();
    index_.clear();
    edges_.clear();
    external_.clear();
    for (const auto& f : files) {
        index_.emplace(f.file, static_cast<std::uint32_t>(files_.size()));
        files_.push_back(f.file);
    }
    adjacency_.assign(files_.size(), {});

    ImportResolver resolver(files_);
    for (std::uint32_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        std::set<std::uint32_t> seen;
        for (const auto& imp : f.imports) {
            auto targets = resolver.resolve(f.file, f.language, imp);
            if (targets.empty()) {
                external_.push_back({i, imp.module.empty() ? (imp.names.empty() ? "" : imp.names.front()) : imp.module});
                continue;
            }
            for (const auto& t : targets) {
                auto it = index_.find(t);
                if (it == index_.end()) continue;
                edges_.push_back({i, it->second, imp.line, imp.module});
                if (seen.insert(it->second).second) adjacency_[i].push_back(it->second);
            }
        }
    }
}

int DependencyGraph::indexOf(const std::string& file) const {
    auto it = index_.find(file);
    return it == index_.end() ? -1 : static_cast<int>(it->second);
}

std::vector<std::uint32_t> DependencyGraph::dependenciesOf(std::uint32_t file) const {
    return file < adjacency_.size() ? adjacency_[file] : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> DependencyGraph::dependentsOf(std::uint32_t file) const {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < adjacency_.size(); ++i)
        if (std::find(adjacency_[i].begin(), adjacency_[i].end(), file) != adjacency_[i].end()) out.push_back(i);
    return out;
}

bool DependencyGraph::dependsOn(std::uint32_t from, std::uint32_t to) const {
    if (from >= adjacency_.size()) return false;
    return std::find(adjacency_[from].begin(), adjacency_[from].end(), to) != adjacency_[from].end();
}

std::vector<std::vector<std::uint32_t>> DependencyGraph::cycles() const {
    std::vector<std::vector<std::uint32_t>> out;
    for (auto& scc : stronglyConnectedComponents(adjacency_)) {
        if (scc.size() > 1) {
            std::sort(scc.begin(), scc.end());
            out.push_back(std::move(scc));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace codetrace
