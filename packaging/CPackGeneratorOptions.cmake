# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Read by CPack once per generator (CPACK_PROJECT_CONFIG_FILE). Archives
# unpack into one named folder; DEB and RPM install at their package paths
# (/usr/bin, /usr/share), never under such a folder.
if(CPACK_INSTALL_CMAKE_PROJECTS)
  if(NOT "${CPACK_INSTALL_CMAKE_PROJECTS}" STREQUAL "${CPACK_CKTOOLS_INSTALL_PROJECT}")
    message(FATAL_ERROR "Binary package must stage its own configured build tree")
  endif()
  list(GET CPACK_INSTALL_CMAKE_PROJECTS 0 _cktools_producer)
  load_cache("${_cktools_producer}" READ_WITH_PREFIX _cktools_package_
    BUILD_TESTING CK_CORE_FS_TEST_NATIVE_CLOSE_HOOKS CK_CORE_FS_TEST_JOURNAL_TEMPORARY_CLEANUP)
  if(NOT "${_cktools_package_BUILD_TESTING}" STREQUAL "OFF" OR
     NOT "${_cktools_package_CK_CORE_FS_TEST_NATIVE_CLOSE_HOOKS}" STREQUAL "OFF" OR
     NOT "${_cktools_package_CK_CORE_FS_TEST_JOURNAL_TEMPORARY_CLEANUP}" STREQUAL "OFF")
    message(FATAL_ERROR "Binary packages require a fresh BUILD_TESTING=OFF build with backend test options OFF")
  endif()
  if(CPACK_CKTOOLS_CHECK_COMPILE_PROFILE)
    file(READ "${_cktools_producer}/compile_commands.json" _cktools_commands)
    string(JSON _cktools_count LENGTH "${_cktools_commands}")
    string(REPLACE "\\" "/" _cktools_paths "${_cktools_commands}")
    if(NOT _cktools_paths MATCHES "\"file\"[^\n]*/src/+files/+native_archive_write_job[.]cpp\"" OR
       NOT _cktools_paths MATCHES "\"file\"[^\n]*/src/+vision/+files/+main[.]cpp\"")
      message(FATAL_ERROR "Production compile database is missing backend and ck-files inputs")
    endif()
    if(_cktools_commands MATCHES "CK_FS_(INTERNAL_TEST_HOOKS|LEGACY_NATIVE_TEST_OBSERVERS|NATIVE_CLOSE_TEST_HOOKS|ACTION_FINAL_DRAIN_TEST_HOOKS|ARCHIVE_READBACK_FAILURE_TEST)")
      message(FATAL_ERROR "Private test hook found in production compile database")
    endif()
    file(SHA256 "${_cktools_producer}/compile_commands.json" _cktools_command_hash)
    message(STATUS "Production package compile profile: ${_cktools_count} commands, SHA256 ${_cktools_command_hash}")
  endif()
  file(SHA256 "${_cktools_producer}/CMakeCache.txt" _cktools_cache_hash)
  message(STATUS "Production package cache: ${_cktools_producer}, SHA256 ${_cktools_cache_hash}")
endif()

if(CPACK_GENERATOR MATCHES "^(TGZ|ZIP)$")
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
else()
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif()
