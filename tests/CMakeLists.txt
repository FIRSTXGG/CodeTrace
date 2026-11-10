find_package(GTest QUIET)
if(NOT GTest_FOUND)
    if(NOT CODETRACE_FETCH_DEPS)
        message(WARNING "GoogleTest not found and fetching disabled: tests are not built")
        return()
    endif()
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(codetrace_tests
    test_json.cpp
    test_cpp_analyzer.cpp
    test_python_analyzer.cpp
    test_js_analyzer.cpp
    test_graph.cpp
    test_project.cpp
    test_rpc.cpp
    test_robustness.cpp
)
target_link_libraries(codetrace_tests PRIVATE codetrace_analyzers GTest::gtest_main)
target_compile_definitions(codetrace_tests PRIVATE CODETRACE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
codetrace_set_warnings(codetrace_tests)

include(GoogleTest)
gtest_discover_tests(codetrace_tests DISCOVERY_TIMEOUT 60)
