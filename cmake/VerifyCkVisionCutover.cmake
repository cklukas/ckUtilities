# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CKTOOLS_INSTALL_PREFIX OR "${CKTOOLS_INSTALL_PREFIX}" STREQUAL "")
  message(FATAL_ERROR "CKTOOLS_INSTALL_PREFIX is required for cutover verification")
endif()

set(_cktools_product_binaries
    ck-utilities
    ck-json-view
    ck-find
    ck-du
    ck-config
    ck-edit
    ck-chat)

foreach(_cktools_binary IN LISTS _cktools_product_binaries)
  set(_cktools_binary_path "${CKTOOLS_INSTALL_PREFIX}/bin/${_cktools_binary}")
  if(NOT EXISTS "${_cktools_binary_path}")
    message(FATAL_ERROR "Missing cutover product binary: ${_cktools_binary_path}")
  endif()

  if(APPLE)
    execute_process(
      COMMAND otool -L "${_cktools_binary_path}"
      RESULT_VARIABLE _cktools_link_result
      OUTPUT_VARIABLE _cktools_link_output
      ERROR_VARIABLE _cktools_link_error)
  elseif(UNIX)
    execute_process(
      COMMAND ldd "${_cktools_binary_path}"
      RESULT_VARIABLE _cktools_link_result
      OUTPUT_VARIABLE _cktools_link_output
      ERROR_VARIABLE _cktools_link_error)
  else()
    set(_cktools_link_result 0)
    set(_cktools_link_output "")
  endif()
  if(NOT _cktools_link_result EQUAL 0)
    message(FATAL_ERROR
      "Unable to inspect cutover binary linkage for ${_cktools_binary}: ${_cktools_link_error}")
  endif()
  string(TOLOWER "${_cktools_link_output}" _cktools_link_output)
  if(_cktools_link_output MATCHES "tvision|ncurses|curses")
    message(FATAL_ERROR "Legacy UI runtime appears in ${_cktools_binary}'s linkage")
  endif()
endforeach()

foreach(_cktools_forbidden_path
    "${CKTOOLS_INSTALL_PREFIX}/include/tvision"
    "${CKTOOLS_INSTALL_PREFIX}/lib/libtvision.a"
    "${CKTOOLS_INSTALL_PREFIX}/lib/cmake/tvision")
  if(EXISTS "${_cktools_forbidden_path}")
    message(FATAL_ERROR "Legacy UI artifact was installed: ${_cktools_forbidden_path}")
  endif()
endforeach()

set(_cktools_public_headers)
file(GLOB_RECURSE _cktools_ck_headers LIST_DIRECTORIES false "${CKTOOLS_INSTALL_PREFIX}/include/ck/*.h"
                                                    "${CKTOOLS_INSTALL_PREFIX}/include/ck/*.hpp")
list(APPEND _cktools_public_headers ${_cktools_ck_headers}
     "${CKTOOLS_INSTALL_PREFIX}/include/json_view_core.hpp"
     "${CKTOOLS_INSTALL_PREFIX}/include/disk_usage_core.hpp"
     "${CKTOOLS_INSTALL_PREFIX}/include/disk_usage_options.hpp")
foreach(_cktools_header IN LISTS _cktools_public_headers)
  if(NOT EXISTS "${_cktools_header}")
    continue()
  endif()
  file(READ "${_cktools_header}" _cktools_header_text)
  string(TOLOWER "${_cktools_header_text}" _cktools_header_text)
  if(_cktools_header_text MATCHES "tvision|turbo vision|ncurses|curses")
    message(FATAL_ERROR "Legacy UI reference in installed public header: ${_cktools_header}")
  endif()
endforeach()

message(STATUS "ckVision cutover verification passed for ${CKTOOLS_INSTALL_PREFIX}")
