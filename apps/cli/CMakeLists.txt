add_executable(codetrace main.cpp)
target_link_libraries(codetrace PRIVATE codetrace_analyzers)
codetrace_set_warnings(codetrace)
install(TARGETS codetrace RUNTIME DESTINATION bin)
