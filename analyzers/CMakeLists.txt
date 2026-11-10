# Language analyzers. Each implements codetrace::ILanguageAnalyzer.
add_library(codetrace_analyzers
    registry.cpp
    common/lexer.cpp
    common/process.cpp
    cpp/heuristic_cpp_analyzer.cpp
    cpp/libclang_cpp_analyzer.cpp
    python/heuristic_python_analyzer.cpp
    python/ast_python_analyzer.cpp
    javascript/heuristic_js_analyzer.cpp
    javascript/treesitter_js_analyzer.cpp
)
add_library(CodeTrace::analyzers ALIAS codetrace_analyzers)
target_link_libraries(codetrace_analyzers PUBLIC codetrace_core)
codetrace_set_warnings(codetrace_analyzers)

# --- libclang (C++) ---------------------------------------------------------
set(CODETRACE_HAVE_LIBCLANG OFF PARENT_SCOPE)
if(CODETRACE_WITH_LIBCLANG)
    include(FindLibClang)
    if(LIBCLANG_FOUND)
        target_include_directories(codetrace_analyzers SYSTEM PRIVATE ${LIBCLANG_INCLUDE_DIR})
        target_link_libraries(codetrace_analyzers PRIVATE ${LIBCLANG_LIBRARY})
        target_compile_definitions(codetrace_analyzers PRIVATE CODETRACE_HAVE_LIBCLANG=1)
        set(CODETRACE_HAVE_LIBCLANG ON PARENT_SCOPE)
    else()
        message(STATUS "libclang not found: C++ uses the heuristic analyzer (set LIBCLANG_ROOT to enable)")
    endif()
endif()

# --- Tree-sitter (JavaScript) -----------------------------------------------
set(CODETRACE_HAVE_TREE_SITTER OFF PARENT_SCOPE)
if(CODETRACE_WITH_TREE_SITTER)
    include(TreeSitter)
    if(TARGET codetrace_tree_sitter_js)
        target_link_libraries(codetrace_analyzers PRIVATE codetrace_tree_sitter_js)
        target_compile_definitions(codetrace_analyzers PRIVATE CODETRACE_HAVE_TREE_SITTER=1)
        set(CODETRACE_HAVE_TREE_SITTER ON PARENT_SCOPE)
    endif()
endif()
