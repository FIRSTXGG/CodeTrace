// CodeTrace — language-neutral program model shared by every analyzer and UI.
#pragma once

#include "core/diagnostics/diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codetrace {

enum class Language : std::uint8_t { Unknown, Cpp, Python, JavaScript };

std::string_view toString(Language lang) noexcept;
Language languageFromString(std::string_view name) noexcept;

enum class SymbolKind : std::uint8_t {
    Module,     // file-level pseudo symbol (top-level code of a script)
    Namespace,
    Class,
    Function,
    Method,
    Constructor,
    Lambda,
};

std::string_view toString(SymbolKind kind) noexcept;
bool isCallable(SymbolKind kind) noexcept;

struct SourceRange {
    std::uint32_t line = 0;     // 1-based, 0 = unknown
    std::uint32_t column = 0;   // 1-based, 0 = unknown
    std::uint32_t endLine = 0;
    std::uint32_t endColumn = 0;
};

using SymbolId = std::uint32_t;
inline constexpr SymbolId kInvalidSymbol = 0xFFFFFFFFu;

struct Symbol {
    SymbolId id = kInvalidSymbol;          // assigned by the project model
    std::string name;                      // short name: "push_back"
    std::string qualifiedName;             // "std::vector::push_back", "pkg.mod.Class.method"
    std::string container;                 // enclosing class / namespace, may be empty
    SymbolKind kind = SymbolKind::Function;
    Language language = Language::Unknown;
    std::string file;                      // normalized absolute path
    SourceRange range;
    std::string signature;                 // best-effort, for display
};

struct CallSite {
    std::string callerQualifiedName;       // qualifiedName of the enclosing callable
    std::string calleeName;                // as written: "foo", "ns::foo", "obj.method" -> "method"
    std::string receiver;                  // "self", "this", "obj", "" if none
    bool isMemberCall = false;             // a.b(), a->b(), self.b()
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

enum class ImportKind : std::uint8_t { Include, SystemInclude, Module, Relative };

struct Import {
    std::string module;                    // "foo/bar.h", "os.path", "./util"
    std::vector<std::string> names;        // imported names, if any
    ImportKind kind = ImportKind::Module;
    int relativeLevel = 0;                 // python "from .. import x" -> 2
    std::uint32_t line = 0;
};

/// A name exported from one language to another (e.g. pybind11 `m.def("name", &fn)`).
struct LanguageBinding {
    Language fromLanguage = Language::Unknown;  // where the implementation lives
    Language toLanguage = Language::Unknown;    // where it becomes callable
    std::string exportedName;                   // name visible in the target language
    std::string targetName;                     // implementation symbol name
    std::uint32_t line = 0;
};

/// Everything an analyzer extracts from one file.
struct FileAnalysis {
    std::string file;
    Language language = Language::Unknown;
    std::string backend;                   // "libclang", "heuristic", "python-ast", ...
    std::vector<Symbol> symbols;
    std::vector<CallSite> calls;
    std::vector<Import> imports;
    std::vector<LanguageBinding> bindings;
    std::vector<Diagnostic> diagnostics;
    bool partial = false;                  // true if the file could only be partially analyzed
};

}  // namespace codetrace
