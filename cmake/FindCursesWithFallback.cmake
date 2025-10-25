include_guard(GLOBAL)

# If curses libraries are already configured, avoid re-running the search.
if(DEFINED CURSES_LIBRARIES AND CURSES_LIBRARIES)
  if(NOT DEFINED CKTOOLS_CURSES_WIDE)
    set(CKTOOLS_CURSES_WIDE "${CURSES_NEED_WIDE}")
  endif()
  return()
endif()

# Attempt to locate wide-character curses first.
set(CURSES_NEED_WIDE TRUE)
find_package(Curses QUIET)

if(CURSES_FOUND)
  set(CKTOOLS_CURSES_WIDE TRUE CACHE BOOL "Wide-character curses support is available" FORCE)
else()
  message(STATUS "Wide-character curses not found; retrying without CURSES_NEED_WIDE")
  set(CURSES_NEED_WIDE FALSE)
  find_package(Curses REQUIRED)
  set(CKTOOLS_CURSES_WIDE FALSE CACHE BOOL "Wide-character curses support is available" FORCE)
endif()

# On macOS runners the headers can live inside the SDK without being exposed
# through the default include paths, so probe the SDK explicitly when needed.
if((NOT CURSES_INCLUDE_PATH OR CURSES_INCLUDE_PATH MATCHES "-NOTFOUND") AND APPLE)
  set(_cktools_curses_search_paths)
  if(CMAKE_OSX_SYSROOT)
    list(APPEND _cktools_curses_search_paths
      "${CMAKE_OSX_SYSROOT}/usr/include"
      "${CMAKE_OSX_SYSROOT}/usr/include/ncurses"
      "${CMAKE_OSX_SYSROOT}/usr/include/ncursesw")
  endif()
  if(NOT _cktools_curses_search_paths)
    execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE _cktools_sdk_path
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(_cktools_sdk_path)
      list(APPEND _cktools_curses_search_paths
        "${_cktools_sdk_path}/usr/include"
        "${_cktools_sdk_path}/usr/include/ncurses"
        "${_cktools_sdk_path}/usr/include/ncursesw")
    endif()
  endif()
  if(_cktools_curses_search_paths)
    find_path(CURSES_INCLUDE_PATH
      NAMES ncurses.h curses.h
      HINTS ${_cktools_curses_search_paths}
      PATH_SUFFIXES ncurses ncursesw
    )
  endif()
endif()

# Normalise include directories so downstream code can rely on CURSES_INCLUDE_DIRS.
if(CURSES_INCLUDE_PATH AND CURSES_INCLUDE_PATH MATCHES "/ncursesw$")
  get_filename_component(_cktools_curses_parent "${CURSES_INCLUDE_PATH}" DIRECTORY)
  set(CURSES_INCLUDE_PATH "${_cktools_curses_parent}" CACHE PATH "" FORCE)
  set(CURSES_INCLUDE_DIR "${_cktools_curses_parent}" CACHE PATH "" FORCE)
  set(CURSES_INCLUDE_DIRS "${_cktools_curses_parent}" CACHE PATH "" FORCE)
elseif(CURSES_INCLUDE_PATH)
  set(CURSES_INCLUDE_PATH "${CURSES_INCLUDE_PATH}" CACHE PATH "" FORCE)
  if(CURSES_INCLUDE_DIR)
    set(CURSES_INCLUDE_DIR "${CURSES_INCLUDE_DIR}" CACHE PATH "" FORCE)
  else()
    set(CURSES_INCLUDE_DIR "${CURSES_INCLUDE_PATH}" CACHE PATH "" FORCE)
  endif()
  if(CURSES_INCLUDE_DIRS)
    set(CURSES_INCLUDE_DIRS "${CURSES_INCLUDE_DIRS}" CACHE STRING "" FORCE)
  else()
    set(CURSES_INCLUDE_DIRS "${CURSES_INCLUDE_DIR}" CACHE STRING "" FORCE)
  endif()
endif()

unset(_cktools_curses_parent)
unset(_cktools_curses_search_paths)
unset(_cktools_sdk_path)
