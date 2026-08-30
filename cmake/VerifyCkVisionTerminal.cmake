cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required
    CKTOOLS_INSTALL_PREFIX
    CKTOOLS_TERMINAL_CONFIG_ROOT
    CKTOOLS_TERMINAL_SMOKE_EXECUTABLE)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for terminal-profile verification")
  endif()
endforeach()

if(NOT EXISTS "${CKTOOLS_TERMINAL_SMOKE_EXECUTABLE}")
  message(FATAL_ERROR "The terminal-smoke helper does not exist: ${CKTOOLS_TERMINAL_SMOKE_EXECUTABLE}")
endif()

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

foreach(_cktools_executable IN LISTS _cktools_native_executables)
  set(_cktools_path "${CKTOOLS_INSTALL_PREFIX}/bin/${_cktools_executable}")
  if(NOT EXISTS "${_cktools_path}")
    message(FATAL_ERROR "Native executable is missing: ${_cktools_path}")
  endif()
  foreach(_cktools_terminal_profile IN LISTS _cktools_terminal_profiles)
    # The helper owns a PTY, resizes it after startup, sends the default Alt+X
    # chord, and compares the terminal mode after the
    # child exits. Its own timeout keeps a failed quit bounded.
    execute_process(
      COMMAND "${CKTOOLS_TERMINAL_SMOKE_EXECUTABLE}"
        --term "${_cktools_terminal_profile}"
        --config-root "${CKTOOLS_TERMINAL_CONFIG_ROOT}"
        -- "${_cktools_path}"
      TIMEOUT 12
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
