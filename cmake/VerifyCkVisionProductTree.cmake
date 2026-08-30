cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CKTOOLS_PRODUCT_PREFIX OR "${CKTOOLS_PRODUCT_PREFIX}" STREQUAL "")
  message(FATAL_ERROR "CKTOOLS_PRODUCT_PREFIX is required for product verification")
endif()

set(_cktools_native_executables
    ck-utilities
    ck-json-view
    ck-find
    ck-du
    ck-config
    ck-edit
    ck-chat)

foreach(_cktools_executable IN LISTS _cktools_native_executables)
  set(_cktools_path "${CKTOOLS_PRODUCT_PREFIX}/bin/${_cktools_executable}")
  if(NOT EXISTS "${_cktools_path}")
    message(FATAL_ERROR "Native executable is missing: ${_cktools_path}")
  endif()
  execute_process(
    COMMAND "${_cktools_path}" --help
    TIMEOUT 20
    RESULT_VARIABLE _cktools_help_result)
  if(NOT _cktools_help_result EQUAL 0)
    message(FATAL_ERROR "Native executable did not complete --help: ${_cktools_executable}")
  endif()
endforeach()

# The launcher must fail before starting a child when a packaged tool is absent.
# Exercise that real composition-root path, then restore the product immediately
# so subsequent cutover checks inspect the complete staged tree.
set(_cktools_launcher_path "${CKTOOLS_PRODUCT_PREFIX}/bin/ck-utilities")

  # Passing --help keeps the check non-interactive while proving that the
  # launcher locates its sibling and propagates the child's exit status.
  foreach(_cktools_child_executable
      ck-json-view
      ck-find
      ck-du
      ck-config
      ck-edit
      ck-chat)
    execute_process(
      COMMAND "${_cktools_launcher_path}" --launch "${_cktools_child_executable}" --help
      TIMEOUT 20
      RESULT_VARIABLE _cktools_child_launch_result
      OUTPUT_VARIABLE _cktools_child_launch_output
      ERROR_VARIABLE _cktools_child_launch_error)
    if(NOT _cktools_child_launch_result EQUAL 0)
      message(FATAL_ERROR
        "Launcher could not start ${_cktools_child_executable}: "
        "${_cktools_child_launch_output}${_cktools_child_launch_error}")
    endif()
  endforeach()

  set(_cktools_missing_tool_path "${CKTOOLS_PRODUCT_PREFIX}/bin/ck-json-view")
  set(_cktools_hidden_tool_path
      "${CKTOOLS_PRODUCT_PREFIX}/bin/ck-json-view.ckutilities-verify-hidden")
  file(RENAME "${_cktools_missing_tool_path}" "${_cktools_hidden_tool_path}"
       RESULT _cktools_hide_result)
  if(NOT _cktools_hide_result STREQUAL "0")
    message(FATAL_ERROR "Could not stage missing-launcher-tool verification: ${_cktools_hide_result}")
  endif()

  execute_process(
    COMMAND "${_cktools_launcher_path}" --launch ck-json-view
    TIMEOUT 20
    RESULT_VARIABLE _cktools_missing_launch_result
    OUTPUT_VARIABLE _cktools_missing_launch_output
    ERROR_VARIABLE _cktools_missing_launch_error)

  file(RENAME "${_cktools_hidden_tool_path}" "${_cktools_missing_tool_path}"
       RESULT _cktools_restore_result)
  if(NOT _cktools_restore_result STREQUAL "0")
    message(FATAL_ERROR "Could not restore staged tool after launcher verification: ${_cktools_restore_result}")
  endif()

  if(_cktools_missing_launch_result EQUAL 0)
    message(FATAL_ERROR "Launcher succeeded even though its selected tool was absent")
  endif()
  set(_cktools_missing_launch_text
      "${_cktools_missing_launch_output}\n${_cktools_missing_launch_error}")
  if(NOT _cktools_missing_launch_text MATCHES "Unable to locate ck-json-view")
    message(FATAL_ERROR "Launcher did not report the missing packaged tool")
  endif()
