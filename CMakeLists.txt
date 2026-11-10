cmake_minimum_required(VERSION 3.21)

project(CodeTrace
    VERSION 0.1.0
    DESCRIPTION "Static analysis, visualization and debugging platform"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CODETRACE_BUILD_DESKTOP     "Build the Qt 6 desktop application"            ON)
option(CODETRACE_BUILD_CLI         "Build the codetrace CLI / JSON-RPC server"     ON)
option(CODETRACE_BUILD_TESTS       "Build GoogleTest unit tests"                   ON)
option(CODETRACE_WITH_LIBCLANG     "Use libclang for precise C++ analysis"         ON)
option(CODETRACE_WITH_TREE_SITTER  "Use Tree-sitter for precise JavaScript analysis" ON)
option(CODETRACE_FETCH_DEPS        "Allow FetchContent for missing dependencies"   ON)
option(CODETRACE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"            OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CodeTraceWarnings)

find_package(Threads REQUIRED)

add_subdirectory(core)
add_subdirectory(analyzers)

if(CODETRACE_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(CODETRACE_BUILD_DESKTOP)
    find_package(Qt6 6.2 QUIET COMPONENTS Widgets Concurrent)
    if(Qt6_FOUND)
        add_subdirectory(apps/desktop-qt)
    else()
        message(WARNING "Qt 6 not found: the desktop application will not be built. "
                        "Set CMAKE_PREFIX_PATH to your Qt installation or pass -DCODETRACE_BUILD_DESKTOP=OFF.")
    endif()
endif()

if(CODETRACE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "CodeTrace ${PROJECT_VERSION}")
message(STATUS "  libclang backend:    ${CODETRACE_HAVE_LIBCLANG}")
message(STATUS "  tree-sitter backend: ${CODETRACE_HAVE_TREE_SITTER}")
message(STATUS "  desktop app:         ${Qt6_FOUND}")
