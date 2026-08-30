cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required CKTOOLS_PACKAGE_ARCHIVE CKTOOLS_PACKAGE_EXTRACT_DIR)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for archive verification")
  endif()
endforeach()

if(NOT EXISTS "${CKTOOLS_PACKAGE_ARCHIVE}")
  message(FATAL_ERROR "ckVision package archive is missing: ${CKTOOLS_PACKAGE_ARCHIVE}")
endif()

file(REMOVE_RECURSE "${CKTOOLS_PACKAGE_EXTRACT_DIR}")
file(MAKE_DIRECTORY "${CKTOOLS_PACKAGE_EXTRACT_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xzf "${CKTOOLS_PACKAGE_ARCHIVE}"
  WORKING_DIRECTORY "${CKTOOLS_PACKAGE_EXTRACT_DIR}"
  RESULT_VARIABLE _cktools_extract_result)
if(NOT _cktools_extract_result EQUAL 0)
  message(FATAL_ERROR "Could not extract ckVision package archive")
endif()

file(GLOB _cktools_archive_entries RELATIVE "${CKTOOLS_PACKAGE_EXTRACT_DIR}"
     "${CKTOOLS_PACKAGE_EXTRACT_DIR}/*")
set(_cktools_archive_roots)
foreach(_cktools_archive_entry IN LISTS _cktools_archive_entries)
  if(IS_DIRECTORY "${CKTOOLS_PACKAGE_EXTRACT_DIR}/${_cktools_archive_entry}")
    list(APPEND _cktools_archive_roots "${_cktools_archive_entry}")
  endif()
endforeach()
list(LENGTH _cktools_archive_roots _cktools_archive_root_count)
list(LENGTH _cktools_archive_entries _cktools_archive_entry_count)
if(NOT _cktools_archive_root_count EQUAL 1 OR
   NOT _cktools_archive_entry_count EQUAL 1)
  message(FATAL_ERROR
    "ckVision package archive must contain exactly one top-level product directory")
endif()

list(GET _cktools_archive_roots 0 _cktools_archive_root)
set(CKTOOLS_PRODUCT_PREFIX "${CKTOOLS_PACKAGE_EXTRACT_DIR}/${_cktools_archive_root}")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyCkVisionProductTree.cmake")

set(CKTOOLS_INSTALL_PREFIX "${CKTOOLS_PRODUCT_PREFIX}")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyCkVisionCutover.cmake")

foreach(_cktools_forbidden_test_path
    "${CKTOOLS_PRODUCT_PREFIX}/include/gtest"
    "${CKTOOLS_PRODUCT_PREFIX}/include/gmock"
    "${CKTOOLS_PRODUCT_PREFIX}/lib/cmake/GTest")
  if(EXISTS "${_cktools_forbidden_test_path}")
    message(FATAL_ERROR
      "Test-framework payload was packaged: ${_cktools_forbidden_test_path}")
  endif()
endforeach()
file(GLOB _cktools_test_libraries
     "${CKTOOLS_PRODUCT_PREFIX}/lib/*gtest*"
     "${CKTOOLS_PRODUCT_PREFIX}/lib/*gmock*")
if(_cktools_test_libraries)
  message(FATAL_ERROR "Test-framework library was packaged: ${_cktools_test_libraries}")
endif()

message(STATUS "ckVision archive verification passed for ${CKTOOLS_PACKAGE_ARCHIVE}")
