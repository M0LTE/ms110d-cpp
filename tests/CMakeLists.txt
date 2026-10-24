include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(Catch2)

add_executable(ms110d_tests
  support/alloc_guard.cpp
  version_test.cpp
  alloc_guard_test.cpp)
target_link_libraries(ms110d_tests PRIVATE m0lte::ms110d Catch2::Catch2WithMain)
target_include_directories(ms110d_tests PRIVATE support)
target_compile_definitions(ms110d_tests PRIVATE
  MS110D_EXPECTED_VERSION="${PROJECT_VERSION}")
ms110d_apply_options(ms110d_tests)

include(Catch)
catch_discover_tests(ms110d_tests)

# Public API lock: see docs/dev/api-lock.md.
set(MS110D_API_SNAPSHOT "${CMAKE_CURRENT_SOURCE_DIR}/api/public-api.txt")
add_test(NAME api_lock
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_INCLUDE=${PROJECT_SOURCE_DIR}/include"
    "-DGENERATED_INCLUDE=${PROJECT_BINARY_DIR}/generated"
    "-DSNAPSHOT=${MS110D_API_SNAPSHOT}"
    "-DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/public-api.actual.txt"
    -P "${PROJECT_SOURCE_DIR}/cmake/ApiLock.cmake")
add_custom_target(update-api-lock
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_INCLUDE=${PROJECT_SOURCE_DIR}/include"
    "-DGENERATED_INCLUDE=${PROJECT_BINARY_DIR}/generated"
    "-DSNAPSHOT=${MS110D_API_SNAPSHOT}"
    -DUPDATE=ON
    -P "${PROJECT_SOURCE_DIR}/cmake/ApiLock.cmake"
  COMMENT "Rewriting the public API snapshot")
