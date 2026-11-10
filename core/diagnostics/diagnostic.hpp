// CodeTrace — diagnostics reported by analyzers and the project pipeline.
#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace codetrace {

enum class Severity : std::uint8_t { Info, Warning, Error };

std::string_view toString(Severity s) noexcept;

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string message;
    std::string source;     // component that produced it ("heuristic-cpp", "project", ...)
};

struct DiagnosticCounts {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t infos = 0;
};

DiagnosticCounts countDiagnostics(const std::vector<Diagnostic>& diagnostics) noexcept;

/// Human readable "file:line:col: severity: message".
std::string format(const Diagnostic& d);

/// Run `fn`, converting any exception into a diagnostic. Returns false if it threw.
template <class Fn>
bool guarded(Fn&& fn, std::vector<Diagnostic>& sink, std::string_view file, std::string_view source) noexcept {
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        try { sink.push_back({Severity::Error, std::string(file), 0, 0, e.what(), std::string(source)}); } catch (...) {}
    } catch (...) {
        try { sink.push_back({Severity::Error, std::string(file), 0, 0, "unknown error", std::string(source)}); } catch (...) {}
    }
    return false;
}

}  // namespace codetrace
