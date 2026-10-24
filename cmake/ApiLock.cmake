# Public API lock. Run with cmake -P.
#   SOURCE_INCLUDE, GENERATED_INCLUDE  the two include roots to scan for *.hpp and *.h
#   SNAPSHOT     the checked-in snapshot file
#   ACTUAL       where to write the current dump when it differs (check mode)
#   UPDATE       ON to rewrite SNAPSHOT instead of comparing
#
# The dump is every public header with // comments, trailing blanks and blank
# lines removed, so reformatting and comment edits do not trip it but any change
# to a declaration does. Public headers must use // comments only.
set(_dump "")
set(_files "")
foreach(_dir "${SOURCE_INCLUDE}" "${GENERATED_INCLUDE}")
  file(GLOB_RECURSE _found RELATIVE "${_dir}" "${_dir}/*.hpp" "${_dir}/*.h")
  foreach(_rel IN LISTS _found)
    list(APPEND _files "${_dir}|${_rel}")
  endforeach()
endforeach()
list(SORT _files)
# Sort by relative path so the order does not depend on which root a file is in.
set(_keyed "")
foreach(_entry IN LISTS _files)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _dir)
  list(GET _parts 1 _rel)
  list(APPEND _keyed "${_rel}|${_dir}")
endforeach()
list(SORT _keyed)

foreach(_entry IN LISTS _keyed)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _rel)
  list(GET _parts 1 _dir)
  file(READ "${_dir}/${_rel}" _text)
  string(REPLACE "\r" "" _text "${_text}")
  string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
  # The version numbers change every release; the declaration is what is locked.
  string(REGEX REPLACE "library_version\\{[0-9, ]*\\}" "library_version{N}" _text "${_text}")
  string(REGEX REPLACE "[ \t]+\n" "\n" _text "${_text}")
  string(REGEX REPLACE "\n\n+" "\n" _text "${_text}")
  string(REGEX REPLACE "^\n+" "" _text "${_text}")
  string(APPEND _dump "==== ${_rel}\n${_text}")
  if(NOT _dump MATCHES "\n$")
    string(APPEND _dump "\n")
  endif()
endforeach()

if(UPDATE)
  file(WRITE "${SNAPSHOT}" "${_dump}")
  message(STATUS "Wrote ${SNAPSHOT}")
  return()
endif()

file(READ "${SNAPSHOT}" _expected)
if(NOT _dump STREQUAL _expected)
  if(ACTUAL)
    file(WRITE "${ACTUAL}" "${_dump}")
  endif()
  message(FATAL_ERROR
    "The public API differs from ${SNAPSHOT}.\n"
    "Compare with: diff ${SNAPSHOT} ${ACTUAL}\n"
    "If the change is deliberate, run: cmake --build <build-dir> --target update-api-lock")
endif()
