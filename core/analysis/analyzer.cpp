#include "core/analysis/analyzer.hpp"

#include <algorithm>
#include <cctype>

namespace codetrace {

std::vector<FileAnalysis> ILanguageAnalyzer::analyzeBatch(std::span<const SourceFile> files,
                                                          const AnalyzerOptions& options) const {
    std::vector<FileAnalysis> out;
    out.reserve(files.size());
    for (const auto& f : files) out.push_back(analyze(f, options));
    return out;
}

namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}
}  // namespace

void AnalyzerRegistry::registerAnalyzer(std::shared_ptr<ILanguageAnalyzer> analyzer) {
    if (!analyzer) return;
    analyzers_.push_back(std::move(analyzer));
    std::stable_sort(analyzers_.begin(), analyzers_.end(),
                     [](const auto& a, const auto& b) { return a->priority() > b->priority(); });
}

void AnalyzerRegistry::registerExtension(std::string extension, Language lang) {
    extension = lower(extension);
    if (!extension.empty() && extension.front() != '.') extension.insert(extension.begin(), '.');
    for (auto& [ext, l] : extensions_) {
        if (ext == extension) {
            l = lang;
            return;
        }
    }
    extensions_.emplace_back(std::move(extension), lang);
}

std::shared_ptr<ILanguageAnalyzer> AnalyzerRegistry::analyzerFor(Language lang) const {
    for (const auto& a : analyzers_)
        if (a->language() == lang && a->isAvailable()) return a;
    return nullptr;
}

std::shared_ptr<ILanguageAnalyzer> AnalyzerRegistry::fallbackFor(Language lang, const ILanguageAnalyzer* current) const {
    bool passed = current == nullptr;
    for (const auto& a : analyzers_) {
        if (a->language() != lang || !a->isAvailable()) continue;
        if (passed) return a;
        if (a.get() == current) passed = true;
    }
    return nullptr;
}

Language AnalyzerRegistry::detectLanguage(std::string_view path, std::string_view firstLine) const {
    auto slash = path.find_last_of("/\\");
    std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) {
        std::string ext = lower(base.substr(dot));
        for (const auto& [e, l] : extensions_)
            if (e == ext) return l;
        return Language::Unknown;
    }
    // Extension-less: look at the shebang.
    if (firstLine.size() > 2 && firstLine[0] == '#' && firstLine[1] == '!') {
        if (firstLine.find("python") != std::string_view::npos) return Language::Python;
        if (firstLine.find("node") != std::string_view::npos) return Language::JavaScript;
    }
    return Language::Unknown;
}

}  // namespace codetrace
