cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required
    CKTOOLS_INSTALL_PREFIX
    CKTOOLS_TERMINAL_CONFIG_ROOT
    CKTOOLS_SCRIPT_EXECUTABLE)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for terminal-profile verification")
  endif()
endforeach()

if(NOT EXISTS "${CKTOOLS_SCRIPT_EXECUTABLE}")
  message(FATAL_ERROR "The PTY helper does not exist: ${CKTOOLS_SCRIPT_EXECUTABLE}")
endif()

function(_cktools_shell_quote _cktools_value _cktools_output)
  string(REPLACE "'" "'\"'\"'" _cktools_escaped "${_cktools_value}")
  set(${_cktools_output} "'${_cktools_escaped}'" PARENT_SCOPE)
endfunction()

set(_cktools_native_executables
    ck-utilities
    ck-json-view
    ck-find
    ck-du
    ck-config
    ck-edit
    ck-chat)
set(_cktools_terminal_profiles xterm-256color xterm vt100)

file(REMOVE_RECURSE "${CKTOOLS_TERMINAL_CONFIG_ROOT}")
file(MAKE_DIRECTORY "${CKTOOLS_TERMINAL_CONFIG_ROOT}")

_cktools_shell_quote("${CKTOOLS_SCRIPT_EXECUTABLE}" _cktools_script_command)
foreach(_cktools_executable IN LISTS _cktools_native_executables)
  set(_cktools_path "${CKTOOLS_INSTALL_PREFIX}/bin/${_cktools_executable}")
  if(NOT EXISTS "${_cktools_path}")
    message(FATAL_ERROR "Native executable is missing: ${_cktools_path}")
  endif()
  _cktools_shell_quote("${_cktools_path}" _cktools_product_command)

  foreach(_cktools_terminal_profile IN LISTS _cktools_terminal_profiles)
    # script allocates a PTY. Feed the default Alt+X chord after the child has
    # entered raw terminal mode; CMake's timeout turns a lost input or failed
    # quit into a bounded diagnostic rather than a hung release job.
    if(APPLE)
      set(_cktools_pty_command
          "(sleep 1; printf '\\033x') | ${_cktools_script_command} -q /dev/null ${_cktools_product_command}")
    else()
      set(_cktools_pty_command
          "(sleep 1; printf '\\033x') | ${_cktools_script_command} -q -c ${_cktools_product_command} /dev/null")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env
        "TERM=${_cktools_terminal_profile}"
        "XDG_CONFIG_HOME=${CKTOOLS_TERMINAL_CONFIG_ROOT}"
        /bin/sh -c "${_cktools_pty_command}"
      TIMEOUT 10
      RESULT_VARIABLE _cktools_terminal_result
      OUTPUT_VARIABLE _cktools_terminal_output
      ERROR_VARIABLE _cktools_terminal_error)
    if(NOT _cktools_terminal_result EQUAL 0)
      file(REMOVE_RECURSE "${CKTOOLS_TERMINAL_CONFIG_ROOT}")
      message(FATAL_ERROR
        "${_cktools_executable} failed the ${_cktools_terminal_profile} PTY smoke: "
        "${_cktools_terminal_output}${_cktools_terminal_error}")
    endif()
  endforeach()
endforeach()

file(REMOVE_RECURSE "${CKTOOLS_TERMINAL_CONFIG_ROOT}")
message(STATUS "ckVision terminal-profile verification passed for ${CKTOOLS_INSTALL_PREFIX}")
