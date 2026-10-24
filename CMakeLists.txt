cmake_minimum_required(VERSION 3.22)

project(m0lte-ms110d VERSION 0.0.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(Ms110dOptions)

# Tests and install rules default on only when this is the top-level project, so
# FetchContent consumers get just the library. Cross builds never build tests.
if(CMAKE_CROSSCOMPILING)
  set(_ms110d_tests_default OFF)
else()
  set(_ms110d_tests_default ${PROJECT_IS_TOP_LEVEL})
endif()
option(MS110D_BUILD_TESTS "Build the unit tests" ${_ms110d_tests_default})
option(MS110D_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})

set(MS110D_PRECISION "double" CACHE STRING "Floating point precision of the modem: double or single")
set_property(CACHE MS110D_PRECISION PROPERTY STRINGS double single)
if(NOT MS110D_PRECISION STREQUAL "double" AND NOT MS110D_PRECISION STREQUAL "single")
  message(FATAL_ERROR "MS110D_PRECISION must be 'double' or 'single', got '${MS110D_PRECISION}'")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

configure_file(
  include/m0lte/ms110d/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/m0lte/ms110d/version.hpp"
  @ONLY)

add_library(ms110d src/version.cpp)
add_library(m0lte::ms110d ALIAS ms110d)

target_compile_features(ms110d PUBLIC cxx_std_20)
target_include_directories(ms110d PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(MS110D_PRECISION STREQUAL "single")
  target_compile_definitions(ms110d PUBLIC MS110D_SINGLE_PRECISION)
endif()
ms110d_apply_options(ms110d)
# The core never throws and never uses RTTI, so it can run on a microcontroller.
target_compile_options(ms110d PRIVATE -fno-exceptions -fno-rtti)

if(MS110D_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MS110D_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS ms110d
    EXPORT m0lte-ms110dTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

  set(MS110D_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/m0lte-ms110d)
  install(EXPORT m0lte-ms110dTargets
    NAMESPACE m0lte::
    DESTINATION ${MS110D_INSTALL_CMAKEDIR})
  configure_package_config_file(
    cmake/m0lte-ms110dConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfig.cmake"
    INSTALL_DESTINATION ${MS110D_INSTALL_CMAKEDIR})
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfigVersion.cmake"
    DESTINATION ${MS110D_INSTALL_CMAKEDIR})
endif()
