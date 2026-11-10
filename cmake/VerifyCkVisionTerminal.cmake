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
    ck-chat
    ck-todo
    ck-sysmon
    ck-files)
set(_cktools_terminal_profiles xterm-256color xterm vt100)

if(DEFINED CKTOOLS_PACKAGE_IMAGE)
  if(NOT IS_ABSOLUTE "${CKTOOLS_INSTALL_PREFIX}" OR IS_SYMLINK "${CKTOOLS_INSTALL_PREFIX}" OR
     IS_SYMLINK "${CKTOOLS_INSTALL_PREFIX}/bin")
    message(FATAL_ERROR "Expected an absolute extracted product prefix with its own bin directory")
  endif()
  file(SHA256 "${CKTOOLS_PACKAGE_IMAGE}" _cktools_package_before)
  if(NOT "${_cktools_package_before}" STREQUAL "${CKTOOLS_PACKAGE_IMAGE_SHA256}")
    message(FATAL_ERROR "Package changed since extraction began: ${CKTOOLS_PACKAGE_IMAGE}")
  endif()
endif()

file(REMOVE_RECURSE "${CKTOOLS_TERMINAL_CONFIG_ROOT}")
file(MAKE_DIRECTORY "${CKTOOLS_TERMINAL_CONFIG_ROOT}")

foreach(_cktools_executable IN LISTS _cktools_native_executables)
  set(_cktools_path "${CKTOOLS_INSTALL_PREFIX}/bin/${_cktools_executable}")
  if(NOT EXISTS "${_cktools_path}")
    message(FATAL_ERROR "Native executable is missing: ${_cktools_path}")
  endif()
  if(DEFINED CKTOOLS_PACKAGE_IMAGE)
    if(IS_SYMLINK "${_cktools_path}" OR IS_DIRECTORY "${_cktools_path}")
      message(FATAL_ERROR "Packaged tool must be its own executable file: ${_cktools_path}")
    endif()
    file(SHA256 "${_cktools_path}" _cktools_image_before)
  endif()
  # ck-todo opens a modal welcome dialog on first run, and ckVision suppresses
  # the standard quit chord while a modal dialog is open (by specification).
  # Its in-memory demo opens straight onto a board and writes nothing, so it
  # exercises the same resize, quit and terminal-mode contract.
  set(_cktools_arguments)
  if(_cktools_executable STREQUAL "ck-todo")
    set(_cktools_arguments --demo)
  endif()
  foreach(_cktools_terminal_profile IN LISTS _cktools_terminal_profiles)
    # The helper owns a PTY, resizes it after startup, sends the default Alt+X
    # chord, and compares the terminal mode after the
    # child exits. Its own timeout keeps a failed quit bounded.
    execute_process(
      COMMAND "${CKTOOLS_TERMINAL_SMOKE_EXECUTABLE}"
        --term "${_cktools_terminal_profile}"
        --config-root "${CKTOOLS_TERMINAL_CONFIG_ROOT}"
        -- "${_cktools_path}" ${_cktools_arguments}
      TIMEOUT 12
      RESULT_VARIABLE _cktools_terminal_result
      OUTPUT_VARIABLE _cktools_terminal_output
      ERROR_VARIABLE _cktools_terminal_error)
    if(NOT "${_cktools_terminal_result}" STREQUAL "0")
      file(REMOVE_RECURSE "${CKTOOLS_TERMINAL_CONFIG_ROOT}")
      message(FATAL_ERROR
        "${_cktools_executable} failed the ${_cktools_terminal_profile} PTY smoke: "
        "${_cktools_terminal_output}${_cktools_terminal_error}")
    endif()
  endforeach()
  if(DEFINED CKTOOLS_PACKAGE_IMAGE)
    file(SHA256 "${_cktools_path}" _cktools_image_after)
    if(NOT "${_cktools_image_before}" STREQUAL "${_cktools_image_after}")
      message(FATAL_ERROR "Extracted tool changed during PTY verification: ${_cktools_path}")
    endif()
    message(STATUS "Packaged PTY image SHA256 ${_cktools_image_after} ${_cktools_path}")
  endif()
endforeach()

file(REMOVE_RECURSE "${CKTOOLS_TERMINAL_CONFIG_ROOT}")
if(DEFINED CKTOOLS_PACKAGE_IMAGE)
  file(SHA256 "${CKTOOLS_PACKAGE_IMAGE}" _cktools_package_after)
  if(NOT "${_cktools_package_after}" STREQUAL "${CKTOOLS_PACKAGE_IMAGE_SHA256}")
    message(FATAL_ERROR "Package changed during PTY verification: ${CKTOOLS_PACKAGE_IMAGE}")
  endif()
  message(STATUS "Verified packaged PTY SHA256 ${_cktools_package_after} ${CKTOOLS_PACKAGE_IMAGE}")
endif()
message(STATUS "ckVision terminal-profile verification passed for ${CKTOOLS_INSTALL_PREFIX}")
