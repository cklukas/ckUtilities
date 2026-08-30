cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required CKTOOLS_BINARY_DIR CKTOOLS_INSTALL_PREFIX)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for installed-product verification")
  endif()
endforeach()

file(REMOVE_RECURSE "${CKTOOLS_INSTALL_PREFIX}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CKTOOLS_BINARY_DIR}" --parallel
  RESULT_VARIABLE _cktools_build_result)
if(NOT _cktools_build_result EQUAL 0)
  message(FATAL_ERROR "ckUtilities build failed before installed-product verification")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CKTOOLS_BINARY_DIR}" --prefix "${CKTOOLS_INSTALL_PREFIX}"
  RESULT_VARIABLE _cktools_install_result)
if(NOT _cktools_install_result EQUAL 0)
  message(FATAL_ERROR "ckUtilities install failed")
endif()

set(CKTOOLS_PRODUCT_PREFIX "${CKTOOLS_INSTALL_PREFIX}")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyCkVisionProductTree.cmake")
