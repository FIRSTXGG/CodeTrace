# Adding a language

1. **Model:** add a value to `codetrace::Language` (`core/analysis/model.hpp`) and
   handle it in `toString` / `languageFromString`.
2. **Analyzer:** implement `ILanguageAnalyzer` in `analyzers/<lang>/`. It must:
   - fill `FileAnalysis::symbols` with `qualifiedName` unique per file. Nested scopes
     are joined with the language's separator (`.` or `::`), and the caller of a call
     site must be one of these names. Top-level code uses a symbol named `<module>` of
     kind `Module`.
   - record calls as written (`calleeName`, plus `receiver` / `isMemberCall` for `a.b()`).
   - record imports as written; teach `ImportResolver::resolve`
     (`core/graph/dependency_graph.cpp`) how to map them to files.
   - never throw for malformed input: report diagnostics and set `partial`.
   - be thread-safe: `analyze` is called concurrently for different files.
3. **Register** it and its extensions in `analyzers/registry.cpp`. A heuristic backend
   with priority 0 plus a precise one with a higher priority gives automatic fallback.
4. **Test:** add `tests/test_<lang>_analyzer.cpp` using `FOR_EACH_BACKEND(Language::X)`
   so every backend has to satisfy the same expectations, and add the language to the
   robustness tests.
