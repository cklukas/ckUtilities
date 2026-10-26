# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED CKTOOLS_SOURCE_ROOT)
  message(FATAL_ERROR "CKTOOLS_SOURCE_ROOT is required")
endif()

set(_tools
  ck-utilities
  ck-json-view
  ck-find
  ck-du
  ck-files
  ck-edit
  ck-chat
  ck-todo
  ck-sysmon
  ck-config)
set(_guides_root "${CKTOOLS_SOURCE_ROOT}/docs/tools")
set(_screenshots_root "${CKTOOLS_SOURCE_ROOT}/docs/generated/screenshots")

file(GLOB _guides RELATIVE "${_guides_root}" "${_guides_root}/*.md")
list(SORT _guides)
set(_expected_guides)
foreach(_tool IN LISTS _tools)
  if(_tool STREQUAL "ck-json-view")
    list(APPEND _expected_guides "json-view.md")
  else()
    list(APPEND _expected_guides "${_tool}.md")
  endif()
endforeach()
list(SORT _expected_guides)
if(NOT _guides STREQUAL _expected_guides)
  message(FATAL_ERROR "Documentation guides do not match the shipped utility set: found='${_guides}' expected='${_expected_guides}'")
endif()

foreach(_guide IN LISTS _guides)
  file(READ "${_guides_root}/${_guide}" _contents)
  if(NOT _contents MATCHES "(^|[\r\n])## Features([ \t\r\n]+[^#\r\n])")
    message(FATAL_ERROR "${_guide} has no non-empty Features section")
  endif()

  string(REGEX MATCHALL "[.][.]/generated/screenshots/[^) \t\r\n]+[.]svg" _references "${_contents}")
  if(NOT _references)
    message(FATAL_ERROR "${_guide} has no embedded generated screenshot")
  endif()
  foreach(_reference IN LISTS _references)
    string(REPLACE "../generated/screenshots/" "" _screenshot "${_reference}")
    if(NOT EXISTS "${_screenshots_root}/${_screenshot}")
      message(FATAL_ERROR "${_guide} references missing screenshot ${_screenshot}")
    endif()
  endforeach()
endforeach()

message(STATUS "Verified ${_guides} utility guides with non-empty feature lists and existing embedded screenshots")
