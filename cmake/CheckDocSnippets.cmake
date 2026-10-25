# Checks that every snippet in an example source appears word for word in the document that shows
# it, so the documentation cannot drift from the code that is compiled and run.
#
# A snippet is the text between "snippet-begin NAME" and "snippet-end NAME" comment lines in an
# example file. Indentation and trailing spaces are ignored on both sides. Usage:
#   cmake "-DPAIRS=<example>|<doc>[,<doc>...];<example>|<doc>..." -P CheckDocSnippets.cmake
# A snippet must appear in at least one of the documents listed for its example.
#
# This works on whole strings, not lists, because code and markdown are full of semicolons and
# square brackets, which CMake lists treat specially.

cmake_minimum_required(VERSION 3.22)

# CMake regex brackets take no escapes, so a newline in a bracket needs the real character.
set(NL "\n")

function(_normalise text out)
  string(REPLACE "\r" "" text "${text}")
  string(REGEX REPLACE "\n[ \t]+" "\n" text "${text}")
  string(REGEX REPLACE "[ \t]+\n" "\n" text "${text}")
  string(REGEX REPLACE "^[ \t]+" "" text "${text}")
  string(REGEX REPLACE "${NL}+$" "" text "${text}")
  set(${out} "${text}${NL}" PARENT_SCOPE)
endfunction()

set(failed OFF)
set(checked 0)
foreach(pair IN LISTS PAIRS)
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 example)
  list(GET parts 1 doc_list)
  string(REPLACE "," ";" doc_files "${doc_list}")

  set(docs_text "")
  foreach(d IN LISTS doc_files)
    file(READ "${d}" t)
    _normalise("${t}" nt)
    string(APPEND docs_text "${nt}")
  endforeach()

  file(READ "${example}" source)
  string(REPLACE "\r" "" source "${source}")
  set(rest "${source}")
  while(TRUE)
    string(REGEX MATCH "snippet-begin ([A-Za-z0-9_]+)[^${NL}]*${NL}" begin "${rest}")
    if(begin STREQUAL "")
      break()
    endif()
    set(name "${CMAKE_MATCH_1}")
    string(FIND "${rest}" "${begin}" at)
    string(LENGTH "${begin}" blen)
    math(EXPR from "${at} + ${blen}")
    string(SUBSTRING "${rest}" ${from} -1 after)
    string(FIND "${after}" "snippet-end ${name}" end)
    if(end EQUAL -1)
      message(SEND_ERROR "snippet '${name}' of ${example} has no end marker")
      set(failed ON)
      break()
    endif()
    string(SUBSTRING "${after}" 0 ${end} body)
    # Drop the partial line holding the end marker's comment opener.
    string(REGEX REPLACE "[^${NL}]+$" "" body "${body}")
    # Other snippet markers nested inside this one are not part of its text.
    string(REGEX REPLACE "[^${NL}]*snippet-(begin|end) [^${NL}]*${NL}" "" body "${body}")
    _normalise("${body}" nb)
    string(FIND "${docs_text}" "${nb}" found)  # nb ends in a newline, as a code line does
    math(EXPR checked "${checked} + 1")
    if(found EQUAL -1)
      message(SEND_ERROR "snippet '${name}' of ${example} is not in the docs word for word")
      set(failed ON)
    endif()
    # Carry on after the begin marker, so a snippet nested in this one is found too.
    set(rest "${after}")
  endwhile()
endforeach()
if(checked EQUAL 0)
  message(SEND_ERROR "no snippets found")
endif()
message(STATUS "${checked} snippets checked")
