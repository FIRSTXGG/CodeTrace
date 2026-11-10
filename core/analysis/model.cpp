#include "core/analysis/model.hpp"

#include <algorithm>
#include <cctype>

namespace codetrace {

std::string_view toString(Language lang) noexcept {
    switch (lang) {
        case Language::Cpp: return "cpp";
        case Language::Python: return "python";
        case Language::JavaScript: return "javascript";
        case Language::Unknown: break;
    }
    return "unknown";
}

Language languageFromString(std::string_view name) noexcept {
    std::string n(name);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (n == "cpp" || n == "c++" || n == "c") return Language::Cpp;
    if (n == "python" || n == "py") return Language::Python;
    if (n == "javascript" || n == "js") return Language::JavaScript;
    return Language::Unknown;
}

std::string_view toString(SymbolKind kind) noexcept {
    switch (kind) {
        case SymbolKind::Module: return "module";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Class: return "class";
        case SymbolKind::Function: return "function";
        case SymbolKind::Method: return "method";
        case SymbolKind::Constructor: return "constructor";
        case SymbolKind::Lambda: return "lambda";
    }
    return "function";
}

bool isCallable(SymbolKind kind) noexcept {
    switch (kind) {
        case SymbolKind::Module:
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Constructor:
        case SymbolKind::Lambda: return true;
        default: return false;
    }
}

}  // namespace codetrace
