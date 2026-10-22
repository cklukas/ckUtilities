# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks the DEB and RPM payload layout: every tool at /usr/bin, the
# documentation under /usr/share, and nothing else - no development files
# and no folder wrapped around the installation paths.
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CKTOOLS_PACKAGE_DIR)
  message(FATAL_ERROR "CKTOOLS_PACKAGE_DIR is required")
endif()

set(_tools ck-utilities ck-json-view ck-find ck-du ck-edit ck-chat ck-todo ck-sysmon ck-config)

function(_cktools_check_layout kind listing)
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
    if(NOT _path MATCHES "^/usr(/bin|/share)?(/.*)?$" OR _path MATCHES "^/usr/(include|lib)(/|$)")
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

foreach(_rpm IN LISTS _rpms)
  execute_process(COMMAND rpm -qlp "${_rpm}" OUTPUT_VARIABLE _listing RESULT_VARIABLE _result ERROR_QUIET)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Could not list ${_rpm}")
  endif()
  _cktools_check_layout("RPM" "${_listing}")
endforeach()

message(STATUS "DEB and RPM layout verification passed in ${CKTOOLS_PACKAGE_DIR}")
