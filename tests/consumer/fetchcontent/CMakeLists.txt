cmake_minimum_required(VERSION 3.22)
project(consumer_fetchcontent LANGUAGES CXX)

include(FetchContent)
# MS110D_SOURCE_DIR points at a checkout; a real consumer would use GIT_REPOSITORY and GIT_TAG.
FetchContent_Declare(m0lte-ms110d SOURCE_DIR "${MS110D_SOURCE_DIR}")
FetchContent_MakeAvailable(m0lte-ms110d)

add_executable(consumer ../main.cpp)
target_link_libraries(consumer PRIVATE m0lte::ms110d)
