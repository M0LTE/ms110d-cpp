cmake_minimum_required(VERSION 3.22)
project(consumer_findpackage LANGUAGES CXX)

find_package(m0lte-ms110d ${EXPECTED_VERSION} CONFIG REQUIRED)

add_executable(consumer ../main.cpp)
target_link_libraries(consumer PRIVATE m0lte::ms110d)
target_compile_definitions(consumer PRIVATE EXPECTED_VERSION="${EXPECTED_VERSION}")
