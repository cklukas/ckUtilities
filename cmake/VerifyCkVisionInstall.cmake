cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required CKTOOLS_BINARY_DIR CKTOOLS_INSTALL_PREFIX)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for installed-product verification")
  endif()
endforeach()

set(_cktools_native_executables
    ck-utilities-ckvision
    ck-json-view-ckvision
    ck-find-ckvision
    ck-du-ckvision
    ck-config-ckvision
    ck-edit-ckvision
    ck-chat-ckvision)

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

foreach(_cktools_executable IN LISTS _cktools_native_executables)
  set(_cktools_path "${CKTOOLS_INSTALL_PREFIX}/bin/${_cktools_executable}")
  if(NOT EXISTS "${_cktools_path}")
    message(FATAL_ERROR "Installed native executable is missing: ${_cktools_path}")
  endif()
  execute_process(
    COMMAND "${_cktools_path}" --help
    TIMEOUT 20
    RESULT_VARIABLE _cktools_help_result)
  if(NOT _cktools_help_result EQUAL 0)
    message(FATAL_ERROR "Installed executable did not complete --help: ${_cktools_executable}")
  endif()
endforeach()
