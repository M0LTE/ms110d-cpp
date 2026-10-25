# The code in the README and the guides. Every snippet is built and run here, and the snippet
# regions are checked to appear word for word in the document that shows them.

add_executable(docs_quickstart quickstart.cpp)
target_link_libraries(docs_quickstart PRIVATE m0lte::ms110d)
ms110d_apply_options(docs_quickstart)
add_test(NAME docs_quickstart COMMAND docs_quickstart)

add_executable(docs_guides guides.cpp)
target_link_libraries(docs_guides PRIVATE m0lte::ms110d)
ms110d_apply_options(docs_guides)
add_test(NAME docs_guides COMMAND docs_guides)

add_test(NAME docs_snippets_in_sync
  COMMAND ${CMAKE_COMMAND}
    "-DPAIRS=${CMAKE_CURRENT_SOURCE_DIR}/quickstart.cpp|${PROJECT_SOURCE_DIR}/README.md;${CMAKE_CURRENT_SOURCE_DIR}/quickstart_c.c|${PROJECT_SOURCE_DIR}/docs/c-quickstart.md;${CMAKE_CURRENT_SOURCE_DIR}/guides.cpp|${PROJECT_SOURCE_DIR}/docs/transmitting.md,${PROJECT_SOURCE_DIR}/docs/receiving.md,${PROJECT_SOURCE_DIR}/docs/microcontroller.md"
    -P "${PROJECT_SOURCE_DIR}/cmake/CheckDocSnippets.cmake")

if(MS110D_BUILD_C_SHARED)
  enable_language(C)
  add_executable(docs_quickstart_c quickstart_c.c)
  set_target_properties(docs_quickstart_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(docs_quickstart_c PRIVATE ms110d_c)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(docs_quickstart_c PRIVATE -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wstrict-prototypes)
  endif()
  add_test(NAME docs_quickstart_c COMMAND docs_quickstart_c)
endif()
