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

# The C interface as a shared library, for callers that are not C++ (.NET, Python).
# Not built for cross builds: a microcontroller links the static library directly.
if(CMAKE_CROSSCOMPILING)
  set(_ms110d_c_shared_default OFF)
else()
  set(_ms110d_c_shared_default ${PROJECT_IS_TOP_LEVEL})
endif()
option(MS110D_BUILD_C_SHARED "Build the C interface as a shared library (ms110d_c)" ${_ms110d_c_shared_default})

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

add_library(ms110d src/version.cpp src/mode.cpp src/tables.cpp src/trinomial_scrambler.cpp src/scrambler_interleaver_check.cpp
  src/puncture.cpp src/fec/tail_biting_encoder.cpp src/fec/tail_biting_viterbi.cpp src/fec/tail_biting_siso.cpp src/bcjr.cpp
  src/dsp/filter_design.cpp src/equaliser.cpp src/modulator.cpp src/framing.cpp src/preamble.cpp src/preamble_detector.cpp src/burst.cpp src/transmitter.cpp src/wn0_receiver.cpp
  src/dfe_receiver.cpp src/dfe_turbo.cpp src/mfb_block_decoder.cpp src/receiver.cpp)
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
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ms110d PRIVATE -fno-exceptions -fno-rtti)
endif()

if(MS110D_BUILD_C_SHARED)
  # The shared library's SOVERSION is the C ABI version in the header, not the library version.
  file(STRINGS include/m0lte/ms110d/ms110d.h _abi_line REGEX "^#define MS110D_ABI_VERSION [0-9]+u")
  string(REGEX REPLACE ".* ([0-9]+)u.*" "\\1" MS110D_ABI_VERSION_NUMBER "${_abi_line}")
  if(NOT MS110D_ABI_VERSION_NUMBER MATCHES "^[0-9]+$")
    message(FATAL_ERROR "could not read MS110D_ABI_VERSION from the C header")
  endif()
  # Only the functions marked MS110D_API are exported; everything else is hidden.
  # The static C++ library is linked into the shared one, so it needs PIC.
  set_target_properties(ms110d PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  add_library(ms110d_c SHARED src/c_api.cpp)
  add_library(m0lte::ms110d_c ALIAS ms110d_c)
  target_link_libraries(ms110d_c PRIVATE ms110d)
  target_include_directories(ms110d_c PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_compile_definitions(ms110d_c PRIVATE MS110D_C_BUILDING)
  set_target_properties(ms110d_c PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${MS110D_ABI_VERSION_NUMBER}
    LINKER_LANGUAGE CXX)
  ms110d_apply_options(ms110d_c)
  # Keep the standard library's templates out of the export table where the linker can.
  include(CheckLinkerFlag)
  set(_ms110d_map_flag "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/ms110d_c.map")
  check_linker_flag(CXX "${_ms110d_map_flag}" MS110D_LINKER_HAS_VERSION_SCRIPT)
  if(MS110D_LINKER_HAS_VERSION_SCRIPT)
    target_link_options(ms110d_c PRIVATE "${_ms110d_map_flag}")
    set_property(TARGET ms110d_c APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ms110d_c.map")
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ms110d_c PRIVATE -fno-exceptions -fno-rtti)
  endif()
endif()

if(MS110D_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MS110D_INSTALL)
  include(CMakePackageConfigHelpers)

  set(_ms110d_install_targets ms110d)
  if(MS110D_BUILD_C_SHARED)
    list(APPEND _ms110d_install_targets ms110d_c)
  endif()
  install(TARGETS ${_ms110d_install_targets}
    EXPORT m0lte-ms110dTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
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
  # SameMinorVersion until 1.0, then switch to SameMajorVersion.
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/m0lte-ms110dConfigVersion.cmake"
    DESTINATION ${MS110D_INSTALL_CMAKEDIR})
endif()
