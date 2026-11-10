# CodeTrace Core: GUI- and IDE-independent analysis engine.
add_library(codetrace_core
    analysis/model.cpp
    analysis/analyzer.cpp
    analysis/project.cpp
    graph/call_graph.cpp
    graph/dependency_graph.cpp
    graph/layout.cpp
    diagnostics/diagnostic.cpp
    protocol/json.cpp
    protocol/serialize.cpp
    protocol/rpc.cpp
)
add_library(CodeTrace::core ALIAS codetrace_core)

# Headers are included as "core/<module>/<file>.hpp" relative to the repository root.
target_include_directories(codetrace_core PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>)
target_compile_features(codetrace_core PUBLIC cxx_std_20)
target_link_libraries(codetrace_core PUBLIC Threads::Threads)
codetrace_set_warnings(codetrace_core)
