# Locates the libclang C API (clang-c/Index.h + libclang shared library).
# Hints: LIBCLANG_ROOT, LLVM_ROOT, Homebrew/Linux distro LLVM prefixes, Xcode toolchain.
# Sets LIBCLANG_FOUND, LIBCLANG_INCLUDE_DIR, LIBCLANG_LIBRARY.

set(_hints ${LIBCLANG_ROOT} $ENV{LIBCLANG_ROOT} ${LLVM_ROOT} $ENV{LLVM_ROOT})
if(APPLE)
    list(APPEND _hints /opt/homebrew/opt/llvm /usr/local/opt/llvm)
    execute_process(COMMAND xcode-select -p OUTPUT_VARIABLE _xcode OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_xcode)
        list(APPEND _xc_lib_hints "${_xcode}/Toolchains/XcodeDefault.xctoolchain/usr/lib" "${_xcode}/usr/lib")
    endif()
elseif(WIN32)
    list(APPEND _hints "C:/Program Files/LLVM" "C:/Program Files (x86)/LLVM")
else()
    foreach(v 21 20 19 18 17 16 15 14)
        list(APPEND _hints /usr/lib/llvm-${v} /usr/lib64/llvm${v})
    endforeach()
    list(APPEND _hints /usr/lib/llvm /usr)
endif()

find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS ${_hints} PATH_SUFFIXES include)
find_library(LIBCLANG_LIBRARY NAMES clang libclang HINTS ${_hints} ${_xc_lib_hints} PATH_SUFFIXES lib lib64 bin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibClang DEFAULT_MSG LIBCLANG_LIBRARY LIBCLANG_INCLUDE_DIR)
mark_as_advanced(LIBCLANG_INCLUDE_DIR LIBCLANG_LIBRARY)
