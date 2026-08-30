cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required CKTOOLS_BINARY_DIR CKTOOLS_INSTALL_PREFIX)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for installed-product verification")
  endif()
endforeach()

if(CKTOOLS_CKVISION_CUTOVER)
  set(_cktools_native_executables
      ck-utilities
      ck-json-view
      ck-find
      ck-du
      ck-config
      ck-edit
      ck-chat)
else()
  set(_cktools_native_executables
      ck-utilities-ckvision
      ck-json-view-ckvision
      ck-find-ckvision
      ck-du-ckvision
      ck-config-ckvision
      ck-edit-ckvision
      ck-chat-ckvision)
endif()

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

# The launcher must fail before starting a child when an installed tool is
# absent. Exercise that real composition-root path in the disposable staged
# prefix, then restore the product immediately so later cutover checks inspect
# the complete install tree.
if(CKTOOLS_CKVISION_CUTOVER)
  set(_cktools_launcher_path "${CKTOOLS_INSTALL_PREFIX}/bin/ck-utilities")

  # Exercise the production composition root's successful fork/exec path for
  # every native child.  Passing --help keeps the check non-interactive while
  # still proving that the launcher locates its staged sibling and propagates
  # the child's exit status.
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
        "Installed launcher could not start ${_cktools_child_executable}: "
        "${_cktools_child_launch_output}${_cktools_child_launch_error}")
    endif()
  endforeach()

  set(_cktools_missing_tool_path "${CKTOOLS_INSTALL_PREFIX}/bin/ck-json-view")
  set(_cktools_hidden_tool_path "${CKTOOLS_INSTALL_PREFIX}/bin/ck-json-view.ckutilities-verify-hidden")
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
    message(FATAL_ERROR "Launcher did not report the missing installed tool")
  endif()
endif()
