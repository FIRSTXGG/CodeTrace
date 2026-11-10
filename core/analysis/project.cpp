#include "core/analysis/project.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace codetrace {

std::string normalizePath(std::string_view path) {
    if (path.empty()) return {};
    std::string s = fs::path(std::string(path)).lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();
    return s;
}

std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// ProjectSnapshot
// ---------------------------------------------------------------------------

const Symbol* ProjectSnapshot::symbol(SymbolId id) const {
    return id < symbols.size() ? &symbols[id] : nullptr;
}

std::vector<SymbolId> ProjectSnapshot::findSymbols(std::string_view query, Language lang, bool callablesOnly) const {
    auto lower = [](std::string_view s) {
        std::string r(s);
        std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return r;
    };
    const std::string q = lower(query);
    std::vector<std::pair<int, SymbolId>> scored;
    for (const auto& s : symbols) {
        if (lang != Language::Unknown && s.language != lang) continue;
        if (callablesOnly && !isCallable(s.kind)) continue;
        int score;
        if (q.empty()) {
            score = 3;
        } else {
            std::string n = lower(s.name);
            if (n == q) score = 0;
            else if (n.rfind(q, 0) == 0) score = 1;
            else if (n.find(q) != std::string::npos) score = 2;
            else if (lower(s.qualifiedName).find(q) != std::string::npos) score = 3;
            else continue;
        }
        scored.emplace_back(score, s.id);
    }
    std::stable_sort(scored.begin(), scored.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return symbols[a.second].qualifiedName < symbols[b.second].qualifiedName;
    });
    std::vector<SymbolId> out;
    out.reserve(scored.size());
    for (auto& [_, id] : scored) out.push_back(id);
    return out;
}

std::shared_ptr<ProjectSnapshot> buildSnapshot(std::string root, std::vector<FileAnalysis> files) {
    auto snap = std::make_shared<ProjectSnapshot>();
    snap->root = std::move(root);
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.file < b.file; });

    std::map<Language, LanguageStats> stats;
    for (auto& f : files) {
        auto& st = stats[f.language];
        st.language = f.language;
        ++st.files;
        st.symbols += f.symbols.size();
        for (auto& s : f.symbols) {
            Symbol copy = s;
            copy.id = static_cast<SymbolId>(snap->symbols.size());
            if (copy.file.empty()) copy.file = f.file;
            if (copy.language == Language::Unknown) copy.language = f.language;
            snap->symbols.push_back(std::move(copy));
        }
        snap->diagnostics.insert(snap->diagnostics.end(), f.diagnostics.begin(), f.diagnostics.end());
    }
    for (auto& [_, st] : stats) snap->languages.push_back(st);
    std::sort(snap->languages.begin(), snap->languages.end(),
              [](const auto& a, const auto& b) { return a.files > b.files; });

    snap->files = std::move(files);
    snap->dependencies.build(snap->files);
    snap->callGraph.build(snap->symbols, snap->files, snap->dependencies);
    return snap;
}

// ---------------------------------------------------------------------------
// ProjectAnalyzer
// ---------------------------------------------------------------------------

ProjectAnalyzer::ProjectAnalyzer(std::shared_ptr<AnalyzerRegistry> registry) : registry_(std::move(registry)) {
    if (!registry_) registry_ = std::make_shared<AnalyzerRegistry>();
}

namespace {

bool isBuildDir(const std::string& name) {
    return name == "build" || name.rfind("build-", 0) == 0 || name.rfind("build_", 0) == 0 ||
           name.rfind("cmake-build-", 0) == 0 || name == "CMakeFiles";
}

std::int64_t mtimeOf(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

bool readFile(const std::string& path, std::uint64_t maxBytes, std::string& out, std::string& error) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        error = "cannot open file";
        return false;
    }
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) {
        error = "cannot determine file size";
        return false;
    }
    if (static_cast<std::uint64_t>(size) > maxBytes) {
        error = "file too large (" + std::to_string(static_cast<std::uint64_t>(size)) + " bytes), skipped";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(out.data(), size)) {
        error = "read error";
        return false;
    }
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

bool looksBinary(const std::string& content) {
    std::size_t n = std::min<std::size_t>(content.size(), 8192);
    return std::find(content.begin(), content.begin() + static_cast<std::ptrdiff_t>(n), '\0') !=
           content.begin() + static_cast<std::ptrdiff_t>(n);
}

std::string firstLine(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    std::string line;
    if (in) {
        char buf[256];
        in.read(buf, sizeof buf);
        line.assign(buf, static_cast<std::size_t>(in.gcount()));
        auto nl = line.find('\n');
        if (nl != std::string::npos) line.resize(nl);
    }
    return line;
}

}  // namespace

std::vector<std::string> ProjectAnalyzer::discoverFiles(const ProjectOptions& options,
                                                        std::vector<Diagnostic>* diagnostics) const {
    std::vector<std::string> files;
    std::error_code ec;
    fs::path root(options.root);
    if (!fs::is_directory(root, ec)) {
        if (fs::is_regular_file(root, ec)) {
            files.push_back(normalizePath(fs::absolute(root, ec).generic_string()));
        } else if (diagnostics) {
            diagnostics->push_back({Severity::Error, options.root, 0, 0, "project root does not exist", "project"});
        }
        return files;
    }

    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (diagnostics)
            diagnostics->push_back({Severity::Error, options.root, 0, 0, "cannot read directory: " + ec.message(), "project"});
        return files;
    }
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            if (diagnostics)
                diagnostics->push_back({Severity::Warning, it->path().generic_string(), 0, 0, ec.message(), "project"});
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            bool ignored = std::find(options.ignoreDirs.begin(), options.ignoreDirs.end(), name) != options.ignoreDirs.end() ||
                           (options.ignoreBuildDirs && isBuildDir(name)) || entry.is_symlink(ec);
            if (ignored) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        std::string path = normalizePath(fs::absolute(entry.path(), ec).generic_string());
        Language lang = registry_->detectLanguage(path);
        if (lang == Language::Unknown && entry.path().extension().empty()) lang = registry_->detectLanguage(path, firstLine(path));
        if (lang == Language::Unknown || !registry_->analyzerFor(lang)) continue;
        files.push_back(std::move(path));
    }
    std::sort(files.begin(), files.end());
    return files;
}

FileAnalysis ProjectAnalyzer::analyzeOne(const SourceFile& file, Language lang, const AnalyzerOptions& options) const {
    const ILanguageAnalyzer* tried = nullptr;
    std::string lastError;
    for (auto analyzer = registry_->analyzerFor(lang); analyzer; analyzer = registry_->fallbackFor(lang, tried)) {
        tried = analyzer.get();
        try {
            FileAnalysis r = analyzer->analyze(file, options);
            r.file = file.path;
            r.language = lang;
            if (r.backend.empty()) r.backend = std::string(analyzer->backendName());
            if (!lastError.empty())
                r.diagnostics.push_back({Severity::Info, file.path, 0, 0, "fell back to " + r.backend + ": " + lastError, "project"});
            return r;
        } catch (const std::exception& e) {
            lastError = std::string(analyzer->backendName()) + " failed: " + e.what();
        } catch (...) {
            lastError = std::string(analyzer->backendName()) + " failed with an unknown error";
        }
    }
    FileAnalysis r;
    r.file = file.path;
    r.language = lang;
    r.partial = true;
    r.diagnostics.push_back({Severity::Error, file.path, 0, 0,
                             lastError.empty() ? "no analyzer available" : lastError, "project"});
    return r;
}

std::shared_ptr<const ProjectSnapshot> ProjectAnalyzer::analyze(const ProjectOptions& optionsIn,
                                                                const ProgressCallback& progress,
                                                                const CancellationToken* cancel) {
    ProjectOptions options = optionsIn;
    std::error_code ec;
    options.root = normalizePath(fs::absolute(fs::path(options.root), ec).generic_string());
    if (options.analyzer.projectRoot.empty()) options.analyzer.projectRoot = options.root;

    std::vector<Diagnostic> projectDiagnostics;
    const auto paths = discoverFiles(options, &projectDiagnostics);

    if (options.autoIncludeDirs) {
        // Without a compilation database, header directories are the best guess for -I.
        std::vector<std::string> dirs;
        for (const auto& p : paths) {
            auto ext = fs::path(p).extension().string();
            if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".hxx" && ext != ".inl") continue;
            auto dir = fs::path(p).parent_path().generic_string();
            if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
            if (dirs.size() >= 64) break;
        }
        for (auto& d : dirs)
            if (std::find(options.analyzer.includeDirs.begin(), options.analyzer.includeDirs.end(), d) ==
                options.analyzer.includeDirs.end())
                options.analyzer.includeDirs.push_back(std::move(d));
    }

    // Snapshot the cache so analysis can run without holding the lock.
    std::map<std::string, CacheEntry> cache;
    {
        std::lock_guard lock(mutex_);
        if (cacheRoot_ != options.root) {
            cache_.clear();
            cacheRoot_ = options.root;
        }
        cache = cache_;
    }

    struct Job {
        SourceFile file;
        Language lang;
        std::uint64_t size;
        std::int64_t mtime;
        std::uint64_t hash;
    };
    std::vector<FileAnalysis> results;
    std::map<std::string, CacheEntry> newCache;
    std::map<Language, std::vector<Job>> jobsByLang;

    for (const auto& path : paths) {
        std::uint64_t size = static_cast<std::uint64_t>(fs::file_size(fs::path(path), ec));
        if (ec) { ec.clear(); size = 0; }
        std::int64_t mtime = mtimeOf(path);
        auto cached = cache.find(path);
        if (cached != cache.end() && cached->second.size == size && cached->second.mtime == mtime) {
            results.push_back(cached->second.analysis);
            newCache.emplace(path, cached->second);
            continue;
        }
        std::string content, error;
        if (!readFile(path, options.maxFileBytes, content, error)) {
            projectDiagnostics.push_back({Severity::Warning, path, 0, 0, error, "project"});
            continue;
        }
        if (looksBinary(content)) continue;
        std::uint64_t hash = fnv1a64(content);
        if (cached != cache.end() && cached->second.hash == hash) {
            CacheEntry e = cached->second;
            e.size = size;
            e.mtime = mtime;
            results.push_back(e.analysis);
            newCache.emplace(path, std::move(e));
            continue;
        }
        Language lang = registry_->detectLanguage(path, content.substr(0, content.find('\n')));
        jobsByLang[lang].push_back({SourceFile{path, std::move(content)}, lang, size, mtime, hash});
    }

    // Build work units: batches for analyzers with costly start-up, single files otherwise.
    struct Unit {
        Language lang;
        std::vector<Job*> jobs;
    };
    std::vector<Unit> units;
    std::size_t totalJobs = 0;
    for (auto& [lang, jobs] : jobsByLang) {
        totalJobs += jobs.size();
        auto analyzer = registry_->analyzerFor(lang);
        std::size_t chunk = analyzer ? std::max<std::size_t>(1, analyzer->preferredBatchSize()) : 1;
        for (std::size_t i = 0; i < jobs.size(); i += chunk) {
            Unit u{lang, {}};
            for (std::size_t j = i; j < std::min(jobs.size(), i + chunk); ++j) u.jobs.push_back(&jobs[j]);
            units.push_back(std::move(u));
        }
    }

    std::vector<std::vector<FileAnalysis>> unitResults(units.size());
    std::atomic<std::size_t> nextUnit{0};
    std::atomic<std::size_t> doneFiles{0};
    std::mutex progressMutex;
    const std::size_t total = totalJobs;

    auto worker = [&] {
        while (true) {
            if (cancel && cancel->isCancelled()) return;
            std::size_t u = nextUnit.fetch_add(1);
            if (u >= units.size()) return;
            Unit& unit = units[u];
            auto analyzer = registry_->analyzerFor(unit.lang);
            std::vector<FileAnalysis> out;
            if (unit.jobs.size() > 1 && analyzer) {
                std::vector<SourceFile> files;
                for (Job* j : unit.jobs) files.push_back(j->file);
                try {
                    out = analyzer->analyzeBatch(files, options.analyzer);
                } catch (...) {
                    out.clear();
                }
                if (out.size() != files.size()) {
                    out.clear();
                    for (Job* j : unit.jobs) out.push_back(analyzeOne(j->file, unit.lang, options.analyzer));
                } else {
                    for (std::size_t k = 0; k < out.size(); ++k) {
                        out[k].file = files[k].path;
                        out[k].language = unit.lang;
                        if (out[k].backend.empty()) out[k].backend = std::string(analyzer->backendName());
                    }
                }
            } else {
                for (Job* j : unit.jobs) out.push_back(analyzeOne(j->file, unit.lang, options.analyzer));
            }
            unitResults[u] = std::move(out);
            std::size_t done = doneFiles.fetch_add(unit.jobs.size()) + unit.jobs.size();
            if (progress) {
                std::lock_guard lock(progressMutex);
                progress({done, total, unit.jobs.back()->file.path});
            }
        }
    };

    unsigned threads = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = static_cast<unsigned>(std::min<std::size_t>(threads, std::max<std::size_t>(1, units.size())));
    if (progress) progress({0, total, {}});
    {
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
    }

    const bool cancelled = cancel && cancel->isCancelled();
    std::size_t reanalyzed = 0;
    for (std::size_t u = 0; u < units.size(); ++u) {
        for (std::size_t k = 0; k < unitResults[u].size(); ++k) {
            Job* job = units[u].jobs[k];
            FileAnalysis& fa = unitResults[u][k];
            ++reanalyzed;
            newCache[job->file.path] = CacheEntry{job->size, job->mtime, job->hash, fa};
            results.push_back(std::move(fa));
        }
    }

    auto snap = buildSnapshot(options.root, std::move(results));
    snap->diagnostics.insert(snap->diagnostics.begin(), projectDiagnostics.begin(), projectDiagnostics.end());
    snap->reanalyzedFiles = reanalyzed;
    snap->cancelled = cancelled;

    std::lock_guard lock(mutex_);
    if (!cancelled) {
        cache_ = std::move(newCache);
    } else {
        // Keep what was finished so the next pass is cheaper.
        for (auto& [k, v] : newCache) cache_[k] = std::move(v);
    }
    last_ = snap;
    return snap;
}

void ProjectAnalyzer::invalidate(const std::vector<std::string>& files) {
    std::lock_guard lock(mutex_);
    for (const auto& f : files) cache_.erase(normalizePath(f));
}

void ProjectAnalyzer::clearCache() {
    std::lock_guard lock(mutex_);
    cache_.clear();
}

std::shared_ptr<const ProjectSnapshot> ProjectAnalyzer::lastSnapshot() const {
    std::lock_guard lock(mutex_);
    return last_;
}

}  // namespace codetrace
