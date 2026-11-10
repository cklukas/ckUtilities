# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks the DEB and RPM payload layout: every tool at /usr/bin, the
# documentation under /usr/share, and no development files or wrapper folder.
# RPM's standard build-ID directories/links are checked separately against
# actual file modes and package-owned executable targets before filtering.
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CKTOOLS_PACKAGE_DIR)
  message(FATAL_ERROR "CKTOOLS_PACKAGE_DIR is required")
endif()

set(_tools ck-utilities ck-json-view ck-find ck-du ck-files ck-edit ck-chat ck-todo ck-sysmon ck-config)

function(_cktools_check_layout kind listing)
  if("${listing}" MATCHES ";")
    message(FATAL_ERROR "${kind} package contains an unrepresentable payload name")
  endif()
  string(REPLACE "\n" ";" _paths "${listing}")
  foreach(_tool IN LISTS _tools)
    if(NOT "/usr/bin/${_tool}" IN_LIST _paths)
      message(FATAL_ERROR "${kind} package does not install /usr/bin/${_tool}")
    endif()
  endforeach()
  foreach(_path IN LISTS _paths)
    if(_path STREQUAL "" OR _path STREQUAL "/")
      continue()
    endif()
    if(_path MATCHES "//" OR _path MATCHES "(^|/)[.][.]?(/|$)")
      message(FATAL_ERROR "${kind} package has a non-canonical path: ${_path}")
    endif()
    if(_path STREQUAL "/usr" OR _path STREQUAL "/usr/bin" OR
       _path STREQUAL "/usr/share" OR _path MATCHES "^/usr/share/")
      continue()
    endif()
    string(REGEX REPLACE "^/usr/bin/" "" _tool_name "${_path}")
    if(NOT _path MATCHES "^/usr/bin/[^/]+$" OR NOT _tool_name IN_LIST _tools)
      message(FATAL_ERROR "${kind} package installs outside /usr/bin and /usr/share: ${_path}")
    endif()
  endforeach()
endfunction()

file(GLOB _debs "${CKTOOLS_PACKAGE_DIR}/*.deb")
file(GLOB _rpms "${CKTOOLS_PACKAGE_DIR}/*.rpm")
if(NOT _debs OR NOT _rpms)
  message(FATAL_ERROR "Expected a DEB and an RPM package in ${CKTOOLS_PACKAGE_DIR}")
endif()

foreach(_deb IN LISTS _debs)
  execute_process(COMMAND dpkg-deb --fsys-tarfile "${_deb}" COMMAND tar -t
    OUTPUT_VARIABLE _listing RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Could not list ${_deb}")
  endif()
  # "./usr/bin/x" -> "/usr/bin/x", directories without their trailing slash.
  string(REGEX REPLACE "(^|\n)\\./" "\\1/" _listing "${_listing}")
  string(REGEX REPLACE "/(\n|$)" "\\1" _listing "${_listing}")
  _cktools_check_layout("DEB" "${_listing}")
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(_rpm IN LISTS _rpms)
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/VerifyRpmBuildIds.py" "${_rpm}" ${_tools}
    OUTPUT_VARIABLE _listing RESULT_VARIABLE _result ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Could not validate RPM layout ${_rpm}: ${_error}")
  endif()
  _cktools_check_layout("RPM" "${_listing}")
endforeach()

message(STATUS "DEB and RPM layout verification passed in ${CKTOOLS_PACKAGE_DIR}")
