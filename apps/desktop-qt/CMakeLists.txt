set(CMAKE_AUTOMOC ON)

add_executable(codetrace-desktop WIN32 MACOSX_BUNDLE
    main.cpp
    MainWindow.cpp MainWindow.h
    GraphView.cpp GraphView.h
    CodeView.cpp CodeView.h
)
target_link_libraries(codetrace-desktop PRIVATE codetrace_analyzers Qt6::Widgets Qt6::Concurrent)
codetrace_set_warnings(codetrace-desktop)
set_target_properties(codetrace-desktop PROPERTIES
    OUTPUT_NAME "CodeTrace"
    MACOSX_BUNDLE_BUNDLE_NAME "CodeTrace"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.codetrace.desktop"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
install(TARGETS codetrace-desktop BUNDLE DESTINATION . RUNTIME DESTINATION bin)

qt_add_resources(codetrace-desktop "icons"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/packaging/icons"
    FILES "${PROJECT_SOURCE_DIR}/packaging/icons/codetrace.png")

# MinGW / clang-mingw toolchains don't always honour WIN32_EXECUTABLE: request the GUI subsystem explicitly.
if(WIN32 AND NOT MSVC)
    target_link_options(codetrace-desktop PRIVATE -Wl,--subsystem,windows)
endif()
