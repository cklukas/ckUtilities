# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

if(NOT CMAKE_HOST_APPLE OR NOT CPACK_EXTERNAL_ENABLE_STAGING OR
   NOT CPACK_COMPONENTS_ALL STREQUAL "Runtime")
  message(FATAL_ERROR "The macOS archive requires CPack's staged Runtime component")
endif()
if(NOT CPACK_PACKAGE_FILE_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9_.-]*$")
  message(FATAL_ERROR "Invalid macOS archive name: ${CPACK_PACKAGE_FILE_NAME}")
endif()

set(_runtime "${CPACK_TEMPORARY_DIRECTORY}/Runtime")
if(NOT IS_DIRECTORY "${_runtime}" OR IS_SYMLINK "${_runtime}")
  message(FATAL_ERROR "Missing staged macOS Runtime directory: ${_runtime}")
endif()

# Store the installed paths, contents and modes in a portable archive.
set(_archive "${CPACK_TOPLEVEL_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.tar.gz")
execute_process(COMMAND /usr/bin/tar --format pax
  --no-mac-metadata --no-xattrs --no-acls --no-fflags
  -czf "${_archive}" -C "${CPACK_TEMPORARY_DIRECTORY}"
  -s ",^Runtime,${CPACK_PACKAGE_FILE_NAME}," Runtime
  COMMAND_ERROR_IS_FATAL ANY)
set(CPACK_EXTERNAL_BUILT_PACKAGES "${_archive}")
