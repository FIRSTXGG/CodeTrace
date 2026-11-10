# Provides target codetrace_tree_sitter_js: the Tree-sitter runtime + JavaScript grammar.
# Uses TREE_SITTER_SOURCE_DIR / TREE_SITTER_JS_SOURCE_DIR if set (offline builds),
# otherwise downloads pinned releases with FetchContent (when CODETRACE_FETCH_DEPS=ON).

set(TREE_SITTER_SOURCE_DIR "" CACHE PATH "Local checkout of tree-sitter (offline builds)")
set(TREE_SITTER_JS_SOURCE_DIR "" CACHE PATH "Local checkout of tree-sitter-javascript (offline builds)")

if(NOT TREE_SITTER_SOURCE_DIR OR NOT TREE_SITTER_JS_SOURCE_DIR)
    if(NOT CODETRACE_FETCH_DEPS)
        message(STATUS "Tree-sitter sources not provided and fetching disabled: JavaScript uses the heuristic analyzer")
        return()
    endif()
    include(FetchContent)
    FetchContent_Declare(tree_sitter
        URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.24.7.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(tree_sitter_javascript
        URL https://github.com/tree-sitter/tree-sitter-javascript/archive/refs/tags/v0.23.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # Sources only: we compile them ourselves below (the upstream CMake projects are not used).
    foreach(_dep tree_sitter tree_sitter_javascript)
        FetchContent_GetProperties(${_dep})
        if(NOT ${_dep}_POPULATED)
            if(POLICY CMP0169)
                cmake_policy(PUSH)
                cmake_policy(SET CMP0169 OLD)
                FetchContent_Populate(${_dep})
                cmake_policy(POP)
            else()
                FetchContent_Populate(${_dep})
            endif()
        endif()
    endforeach()
    set(TREE_SITTER_SOURCE_DIR ${tree_sitter_SOURCE_DIR})
    set(TREE_SITTER_JS_SOURCE_DIR ${tree_sitter_javascript_SOURCE_DIR})
endif()

add_library(codetrace_tree_sitter_js STATIC
    ${TREE_SITTER_SOURCE_DIR}/lib/src/lib.c
    ${TREE_SITTER_JS_SOURCE_DIR}/src/parser.c
    ${TREE_SITTER_JS_SOURCE_DIR}/src/scanner.c)
target_include_directories(codetrace_tree_sitter_js
    PUBLIC ${TREE_SITTER_SOURCE_DIR}/lib/include
    PRIVATE ${TREE_SITTER_SOURCE_DIR}/lib/src ${TREE_SITTER_JS_SOURCE_DIR}/src)
set_target_properties(codetrace_tree_sitter_js PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(codetrace_tree_sitter_js PRIVATE -w)
    target_compile_definitions(codetrace_tree_sitter_js PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE)
endif()
