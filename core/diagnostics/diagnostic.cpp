#include "core/diagnostics/diagnostic.hpp"

namespace codetrace {

std::string_view toString(Severity s) noexcept {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "warning";
}

DiagnosticCounts countDiagnostics(const std::vector<Diagnostic>& diagnostics) noexcept {
    DiagnosticCounts c;
    for (const auto& d : diagnostics) {
        switch (d.severity) {
            case Severity::Error: ++c.errors; break;
            case Severity::Warning: ++c.warnings; break;
            case Severity::Info: ++c.infos; break;
        }
    }
    return c;
}

std::string format(const Diagnostic& d) {
    std::string out = d.file.empty() ? std::string("<project>") : d.file;
    if (d.line) {
        out += ":" + std::to_string(d.line);
        if (d.column) out += ":" + std::to_string(d.column);
    }
    out += ": ";
    out += toString(d.severity);
    out += ": " + d.message;
    if (!d.source.empty()) out += " [" + d.source + "]";
    return out;
}

}  // namespace codetrace
