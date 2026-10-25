# The shadow receiver's command line decoder (docs/dev/shadow.md). Off by default: library
# consumers do not build it. Configure with -DMS110D_BUILD_TOOLS=ON.
add_executable(ms110d-decode main.cpp wav.cpp resample.cpp)
target_link_libraries(ms110d-decode PRIVATE m0lte::ms110d)
ms110d_apply_options(ms110d-decode)
install(TARGETS ms110d-decode RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
# The commit the tool reports in its summary line: -DMS110D_TOOL_GIT=..., else asked of git.
set(MS110D_TOOL_GIT "" CACHE STRING "Commit label the tool reports (default: git rev-parse HEAD)")
if(NOT MS110D_TOOL_GIT)
  execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE _git OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  set(MS110D_TOOL_GIT "${_git}")
endif()
if(NOT MS110D_TOOL_GIT)
  set(MS110D_TOOL_GIT unknown)
endif()
target_compile_definitions(ms110d-decode PRIVATE MS110D_TOOL_GIT="${MS110D_TOOL_GIT}")
