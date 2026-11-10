// Precise C++ analysis with libclang (LLVM/Clang C API).
// Compiled only when CODETRACE_HAVE_LIBCLANG is defined; otherwise
// createLibclangAnalyzer() returns nullptr and the heuristic analyzer is used.

#include "analyzers/cpp/cpp_analyzer.hpp"

#ifndef CODETRACE_HAVE_LIBCLANG

namespace codetrace::cpp {
std::shared_ptr<ILanguageAnalyzer> createLibclangAnalyzer() { return nullptr; }
}  // namespace codetrace::cpp

#else

#include <clang-c/Index.h>

#include "analyzers/common/process.hpp"

#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace codetrace::cpp {

namespace {

std::string str(CXString s) {
    const char* c = clang_getCString(s);
    std::string out = c ? c : "";
    clang_disposeString(s);
    return out;
}

/// "ns::Class::method" built from semantic parents (skips the translation unit).
std::string qualifiedName(CXCursor c) {
    std::vector<std::string> parts;
    while (!clang_Cursor_isNull(c) && clang_getCursorKind(c) != CXCursor_TranslationUnit) {
        CXCursorKind k = clang_getCursorKind(c);
        std::string name = str(clang_getCursorSpelling(c));
        if (k == CXCursor_Namespace && name.empty()) name = "(anonymous)";
        if (k == CXCursor_LinkageSpec || k == CXCursor_UnexposedDecl) {
            c = clang_getCursorSemanticParent(c);
            continue;
        }
        if (!name.empty()) parts.push_back(name);
        c = clang_getCursorSemanticParent(c);
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out;
}

bool isFunctionLike(CXCursorKind k) {
    return k == CXCursor_FunctionDecl || k == CXCursor_CXXMethod || k == CXCursor_Constructor ||
           k == CXCursor_Destructor || k == CXCursor_FunctionTemplate || k == CXCursor_ConversionFunction;
}

bool isClassLike(CXCursorKind k) {
    return k == CXCursor_ClassDecl || k == CXCursor_StructDecl || k == CXCursor_UnionDecl ||
           k == CXCursor_ClassTemplate || k == CXCursor_ClassTemplatePartialSpecialization;
}

struct Location {
    std::uint32_t line = 0, column = 0;
    bool mainFile = false;
};

Location locationOf(CXCursor c) {
    Location l;
    CXSourceLocation loc = clang_getCursorLocation(c);
    l.mainFile = clang_Location_isFromMainFile(loc) != 0;
    unsigned line = 0, col = 0;
    clang_getSpellingLocation(loc, nullptr, &line, &col, nullptr);
    l.line = line;
    l.column = col;
    return l;
}

struct Visitor {
    FileAnalysis& out;
    std::vector<std::string> functionStack;   // qualified names of enclosing definitions

    void visitChildren(CXCursor c) {
        clang_visitChildren(
            c,
            [](CXCursor child, CXCursor, CXClientData data) {
                static_cast<Visitor*>(data)->visit(child);
                return CXChildVisit_Continue;
            },
            this);
    }

    void visit(CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        Location loc = locationOf(c);

        if (k == CXCursor_InclusionDirective) {
            if (!loc.mainFile) return;
            Import imp;
            imp.module = str(clang_getCursorSpelling(c));
            imp.line = loc.line;
            // libclang does not expose the bracket style; check the token text.
            CXTranslationUnit tu = clang_Cursor_getTranslationUnit(c);
            CXToken* tokens = nullptr;
            unsigned n = 0;
            clang_tokenize(tu, clang_getCursorExtent(c), &tokens, &n);
            imp.kind = ImportKind::Include;
            for (unsigned i = 0; i < n; ++i) {
                std::string t = str(clang_getTokenSpelling(tu, tokens[i]));
                if (t == "<") { imp.kind = ImportKind::SystemInclude; break; }
            }
            if (tokens) clang_disposeTokens(tu, tokens, n);
            out.imports.push_back(std::move(imp));
            return;
        }

        // Skip everything that does not come from this file (headers are analyzed on their own).
        if (!loc.mainFile && k != CXCursor_TranslationUnit) {
            if (functionStack.empty()) return;
        }

        if (isClassLike(k) && clang_isCursorDefinition(c) && loc.mainFile) {
            Symbol s;
            s.name = str(clang_getCursorSpelling(c));
            if (!s.name.empty()) {
                s.qualifiedName = qualifiedName(c);
                s.container = qualifiedName(clang_getCursorSemanticParent(c));
                s.kind = SymbolKind::Class;
                s.language = Language::Cpp;
                s.file = out.file;
                s.range = range(c);
                s.signature = str(clang_getCursorDisplayName(c));
                out.symbols.push_back(std::move(s));
            }
            visitChildren(c);
            return;
        }

        if (isFunctionLike(k) && clang_isCursorDefinition(c) && loc.mainFile) {
            Symbol s;
            s.name = str(clang_getCursorSpelling(c));
            s.qualifiedName = qualifiedName(c);
            CXCursor parent = clang_getCursorSemanticParent(c);
            s.container = qualifiedName(parent);
            bool member = isClassLike(clang_getCursorKind(parent));
            s.kind = k == CXCursor_Constructor ? SymbolKind::Constructor
                     : (member || k == CXCursor_CXXMethod || k == CXCursor_Destructor) ? SymbolKind::Method
                                                                                         : SymbolKind::Function;
            s.language = Language::Cpp;
            s.file = out.file;
            s.range = range(c);
            CXType type = clang_getCursorType(c);
            s.signature = str(clang_getTypeSpelling(clang_getResultType(type))) + " " + str(clang_getCursorDisplayName(c));
            functionStack.push_back(s.qualifiedName);
            out.symbols.push_back(std::move(s));
            visitChildren(c);
            functionStack.pop_back();
            return;
        }

        if (k == CXCursor_CallExpr && !functionStack.empty()) {
            CXCursor ref = clang_getCursorReferenced(c);
            if (!clang_Cursor_isNull(ref)) {
                CXCursorKind rk = clang_getCursorKind(ref);
                if (isFunctionLike(rk) || rk == CXCursor_Constructor) {
                    CallSite call;
                    call.callerQualifiedName = functionStack.back();
                    std::string q = qualifiedName(ref);
                    if (q.rfind("std::", 0) != 0 && q.rfind("__", 0) != 0 && q.find("(lambda") == std::string::npos) {
                        call.calleeName = q.empty() ? str(clang_getCursorSpelling(ref)) : q;
                        call.isMemberCall = rk == CXCursor_CXXMethod;
                        call.line = loc.line;
                        call.column = loc.column;
                        if (!call.calleeName.empty()) out.calls.push_back(std::move(call));
                    }
                }
            }
        }
        visitChildren(c);
    }

    static SourceRange range(CXCursor c) {
        CXSourceRange r = clang_getCursorExtent(c);
        unsigned l1 = 0, c1 = 0, l2 = 0, c2 = 0;
        clang_getSpellingLocation(clang_getRangeStart(r), nullptr, &l1, &c1, nullptr);
        clang_getSpellingLocation(clang_getRangeEnd(r), nullptr, &l2, &c2, nullptr);
        Location name = locationOf(c);
        return {name.line ? name.line : l1, name.column ? name.column : c1, l2, c2};
    }
};

/// On macOS a standalone libclang does not know the SDK; ask xcrun once.
const std::string& macSdkPath() {
    static std::string path;
#ifdef __APPLE__
    static std::once_flag once;
    std::call_once(once, [] {
        auto r = proc::run({"xcrun", "--show-sdk-path"});
        if (r.started && r.exitCode == 0) {
            path = r.output;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        }
    });
#endif
    return path;
}

class LibclangCppAnalyzer final : public ILanguageAnalyzer {
public:
    LibclangCppAnalyzer() {
        const char* backend = std::getenv("CODETRACE_CPP_BACKEND");
        enabled_ = !(backend && std::string(backend) == "heuristic");
    }

    Language language() const noexcept override { return Language::Cpp; }
    std::string_view backendName() const noexcept override { return "libclang"; }
    int priority() const noexcept override { return 10; }
    bool isAvailable() const noexcept override { return enabled_; }

    FileAnalysis analyze(const SourceFile& file, const AnalyzerOptions& options) const override {
        // Clang's parser recurses on nesting and a stack overflow cannot be
        // recovered from; pathological files go to the iterative analyzer instead.
        if (std::size_t depth = maxNesting(file.content); depth > kMaxNesting) {
            FileAnalysis h = HeuristicCppAnalyzer().analyze(file, options);
            h.diagnostics.push_back({Severity::Info, file.path, 0, 0,
                                     "nesting depth " + std::to_string(depth) + " exceeds libclang limit; used heuristic analysis",
                                     "libclang"});
            return h;
        }
        FileAnalysis out;
        out.file = file.path;
        out.language = Language::Cpp;
        out.backend = "libclang";

        std::vector<std::string> args = {"-x", "c++", "-std=c++20", "-fsyntax-only", "-Wno-everything",
                                         "-ferror-limit=0"};
        auto slash = file.path.find_last_of('/');
        if (slash != std::string::npos) args.push_back("-I" + file.path.substr(0, slash));
        if (!options.projectRoot.empty()) {
            args.push_back("-I" + options.projectRoot);
            args.push_back("-I" + options.projectRoot + "/include");
            args.push_back("-I" + options.projectRoot + "/src");
        }
        for (const auto& d : options.includeDirs) args.push_back("-I" + d);
        if (const std::string& sdk = macSdkPath(); !sdk.empty()) {
            args.push_back("-isysroot");
            args.push_back(sdk);
        }
        for (const auto& d : options.defines) args.push_back("-D" + d);
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());

        CXIndex index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0, /*displayDiagnostics=*/0);
        CXUnsavedFile unsaved{file.path.c_str(), file.content.c_str(), static_cast<unsigned long>(file.content.size())};
        CXTranslationUnit tu = nullptr;
        unsigned flags = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_Incomplete |
                         CXTranslationUnit_KeepGoing;
        CXErrorCode err = clang_parseTranslationUnit2(index, file.path.c_str(), argv.data(), static_cast<int>(argv.size()),
                                                      &unsaved, 1, flags, &tu);
        if (err != CXError_Success || !tu) {
            clang_disposeIndex(index);
            FileAnalysis h = HeuristicCppAnalyzer().analyze(file, options);
            h.partial = true;
            h.diagnostics.push_back({Severity::Warning, file.path, 0, 0,
                                     "libclang could not parse the file (error " + std::to_string(err) + "); used heuristic analysis",
                                     "libclang"});
            return h;
        }

        Visitor v{out, {}};
        v.visitChildren(clang_getTranslationUnitCursor(tu));

        // Collect errors located in this file. Without a build configuration most
        // of them cascade from unresolved #includes, so they are reported softly.
        struct Err { unsigned line, col; std::string msg; };
        std::vector<Err> errors;
        bool missingInclude = false;
        unsigned n = clang_getNumDiagnostics(tu);
        for (unsigned i = 0; i < n; ++i) {
            CXDiagnostic d = clang_getDiagnostic(tu, i);
            if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
                CXSourceLocation loc = clang_getDiagnosticLocation(d);
                if (clang_Location_isFromMainFile(loc)) {
                    unsigned line = 0, col = 0;
                    clang_getSpellingLocation(loc, nullptr, &line, &col, nullptr);
                    std::string msg = str(clang_getDiagnosticSpelling(d));
                    if (msg.find("file not found") != std::string::npos) missingInclude = true;
                    errors.push_back({line, col, std::move(msg)});
                }
            }
            clang_disposeDiagnostic(d);
        }
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);

        // The token analyzer always runs: it supplies language bindings (pybind11)
        // and fills gaps when libclang could not resolve declarations.
        FileAnalysis heuristic = HeuristicCppAnalyzer().analyze(file, options);
        out.bindings = std::move(heuristic.bindings);
        if (!errors.empty()) {
            mergeMissing(out, heuristic);
            out.backend = "libclang+heuristic";
            out.partial = true;
            Severity sev = missingInclude ? Severity::Info : Severity::Warning;
            for (std::size_t i = 0; i < errors.size() && i < 3; ++i)
                out.diagnostics.push_back({sev, file.path, errors[i].line, errors[i].col, errors[i].msg, "libclang"});
            if (errors.size() > 3)
                out.diagnostics.push_back({sev, file.path, 0, 0,
                                           std::to_string(errors.size() - 3) + " more libclang errors" +
                                               (missingInclude ? " (missing includes: pass -I / -D for precise results)" : ""),
                                           "libclang"});
        }
        return out;
    }

    /// Add symbols and calls that the heuristic pass found but libclang did not.
    static void mergeMissing(FileAnalysis& out, const FileAnalysis& h) {
        std::set<std::string> known;
        for (const auto& s : out.symbols) known.insert(s.qualifiedName + "#" + std::to_string(s.range.line));
        std::set<std::string> knownNames;
        for (const auto& s : out.symbols) knownNames.insert(s.qualifiedName);
        for (const auto& s : h.symbols)
            if (!knownNames.count(s.qualifiedName) && !known.count(s.qualifiedName + "#" + std::to_string(s.range.line)))
                out.symbols.push_back(s);
        auto last = [](const std::string& n) {
            auto p = n.rfind("::");
            return p == std::string::npos ? n : n.substr(p + 2);
        };
        std::set<std::pair<std::uint32_t, std::string>> calls;
        for (const auto& c : out.calls) calls.insert({c.line, last(c.calleeName)});
        for (const auto& c : h.calls)
            if (calls.insert({c.line, last(c.calleeName)}).second) out.calls.push_back(c);
        if (out.imports.empty()) out.imports = h.imports;
    }

private:
    static constexpr std::size_t kMaxNesting = 256;

    static std::size_t maxNesting(const std::string& src) {
        std::size_t depth = 0, best = 0;
        for (char c : src) {
            if (c == '(' || c == '[' || c == '{') best = std::max(best, ++depth);
            else if ((c == ')' || c == ']' || c == '}') && depth > 0) --depth;
        }
        return best;
    }

    bool enabled_ = true;
};

}  // namespace

std::shared_ptr<ILanguageAnalyzer> createLibclangAnalyzer() { return std::make_shared<LibclangCppAnalyzer>(); }

}  // namespace codetrace::cpp

#endif
