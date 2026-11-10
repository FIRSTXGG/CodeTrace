#pragma once

#include "analyzers/registry.hpp"
#include "core/analysis/project.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace ct = codetrace;

/// Every available analyzer for `lang` (precise backends and heuristic fallbacks).
inline std::vector<std::shared_ptr<ct::ILanguageAnalyzer>> backendsFor(ct::Language lang) {
    static auto registry = ct::createDefaultRegistry();
    std::vector<std::shared_ptr<ct::ILanguageAnalyzer>> out;
    for (const auto& a : registry->analyzers())
        if (a->language() == lang && a->isAvailable()) out.push_back(a);
    return out;
}

inline ct::FileAnalysis analyzeWith(const ct::ILanguageAnalyzer& a, const std::string& code,
                                    const std::string& path = "/virtual/test") {
    ct::SourceFile f{path, code};
    return a.analyze(f, {});
}

inline const ct::Symbol* findSymbol(const ct::FileAnalysis& fa, const std::string& qualified) {
    for (const auto& s : fa.symbols)
        if (s.qualifiedName == qualified) return &s;
    return nullptr;
}

inline bool hasCall(const ct::FileAnalysis& fa, const std::string& caller, const std::string& calleeSuffix) {
    for (const auto& c : fa.calls) {
        if (c.callerQualifiedName != caller) continue;
        const auto& n = c.calleeName;
        if (n == calleeSuffix) return true;
        if (n.size() > calleeSuffix.size() && n.compare(n.size() - calleeSuffix.size(), calleeSuffix.size(), calleeSuffix) == 0 &&
            (n[n.size() - calleeSuffix.size() - 1] == ':' || n[n.size() - calleeSuffix.size() - 1] == '.'))
            return true;
    }
    return false;
}

inline std::string dumpSymbols(const ct::FileAnalysis& fa) {
    std::string s = "backend=" + fa.backend + "\nsymbols:";
    for (const auto& sym : fa.symbols) s += "\n  " + std::string(ct::toString(sym.kind)) + " " + sym.qualifiedName + " @" + std::to_string(sym.range.line);
    s += "\ncalls:";
    for (const auto& c : fa.calls) s += "\n  " + c.callerQualifiedName + " -> " + c.calleeName + " @" + std::to_string(c.line);
    return s;
}

#define FOR_EACH_BACKEND(lang)                                   \
    for (const auto& backend : backendsFor(lang))                \
        if (SCOPED_TRACE(std::string(backend->backendName())); true)
