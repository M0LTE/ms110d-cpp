# Deviations list check and regenerate. Run with cmake -P.
#   TEST_EXE  the test binary that holds the deviation tests
#   DOC       the checked-in docs/deviations.md
#   ACTUAL    where to write the current list (check mode); always written, then compared with DOC
#   UPDATE    ON to rewrite DOC instead of comparing
if(UPDATE)
  set(_out "${DOC}")
else()
  set(_out "${ACTUAL}")
endif()
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "MS110D_DEVIATIONS_OUT=${_out}" "${TEST_EXE}" "[.deviation-doc]"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _log
  ERROR_VARIABLE _log)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Could not generate the deviations list (exit ${_rc}):\n${_log}")
endif()
if(UPDATE)
  message(STATUS "Wrote ${DOC}")
  return()
endif()

file(READ "${DOC}" _expected)
file(READ "${ACTUAL}" _actual)
if(NOT _actual STREQUAL _expected)
  message(FATAL_ERROR
    "${DOC} is out of date with the deviation tests.\n"
    "Compare with: diff ${DOC} ${ACTUAL}\n"
    "To regenerate, run: cmake --build <build-dir> --target update-deviations")
endif()
