include_guard(GLOBAL)

# Reset any cached results so reconfigures react to dependency changes.
foreach(_cktools_var
    CURSES_FOUND
    CURSES_INCLUDE_PATH
    CURSES_INCLUDE_DIR
    CURSES_INCLUDE_DIRS
    CURSES_LIBRARY
    CURSES_LIBRARIES
    CURSES_CURSES_LIBRARY
    CURSES_NCURSES_LIBRARY
    CURSES_EXTRA_LIBRARY
    CURSES_FORM_LIBRARY
    CURSES_HAVE_CURSES_H
    CURSES_HAVE_NCURSES_H
    CURSES_HAVE_NCURSES_CURSES_H
    CURSES_HAVE_NCURSES_NCURSES_H)
  unset(${_cktools_var})
  unset(${_cktools_var} CACHE)
endforeach()

set(CURSES_NEED_WIDE TRUE)
find_package(Curses QUIET)

if(NOT CURSES_FOUND AND UNIX AND NOT APPLE)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(NCURSESW QUIET ncursesw)
    if(NCURSESW_FOUND)
      find_library(_cktools_ncursesw_lib
        NAMES ncursesw
        HINTS ${NCURSESW_LIBRARY_DIRS})
      find_library(_cktools_tinfo_lib
        NAMES tinfow tinfo
        HINTS ${NCURSESW_LIBRARY_DIRS})
      if(_cktools_ncursesw_lib)
        set(CURSES_FOUND TRUE)
        set(CURSES_INCLUDE_PATH "${NCURSESW_INCLUDE_DIRS}")
        set(CURSES_INCLUDE_DIR "${NCURSESW_INCLUDE_DIRS}")
        set(CURSES_INCLUDE_DIRS "${NCURSESW_INCLUDE_DIRS}")
        set(CURSES_LIBRARY "${_cktools_ncursesw_lib}")
        set(CURSES_LIBRARIES "${_cktools_ncursesw_lib}")
        if(_cktools_tinfo_lib)
          list(APPEND CURSES_LIBRARIES "${_cktools_tinfo_lib}")
        endif()
      endif()
    endif()
  endif()
endif()

if(NOT CURSES_FOUND AND APPLE)
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
  find_library(_cktools_ncurses_lib
    NAMES ncurses
    HINTS ${CMAKE_OSX_SYSROOT}/usr/lib /usr/lib
  )
  if(_cktools_ncurses_lib)
    if(NOT CURSES_INCLUDE_PATH OR CURSES_INCLUDE_PATH MATCHES "-NOTFOUND$")
      find_path(CURSES_INCLUDE_PATH
        NAMES ncurses.h curses.h
        HINTS ${_cktools_curses_search_paths}
        PATH_SUFFIXES ncurses ncursesw
      )
    endif()
    if(CURSES_INCLUDE_PATH AND NOT CURSES_INCLUDE_PATH MATCHES "-NOTFOUND$")
      set(CURSES_FOUND TRUE)
      set(CURSES_LIBRARY "${_cktools_ncurses_lib}")
      set(CURSES_LIBRARIES "${_cktools_ncurses_lib}")
    endif()
  endif()
endif()

if(NOT CURSES_FOUND)
  message(FATAL_ERROR
    "Wide-character ncurses (ncursesw) is required but was not found.\n"
    "Install the development package for ncursesw (e.g. `libncurses-dev` on "
    "Ubuntu, `ncurses` via Homebrew on macOS) and re-run CMake.")
endif()

set(CKTOOLS_CURSES_WIDE TRUE CACHE BOOL "Wide-character curses support is available" FORCE)

# On macOS runners the headers can live inside the SDK without being exposed
# through the default include paths, so probe the SDK explicitly when needed.
if(APPLE AND (NOT CURSES_INCLUDE_PATH OR CURSES_INCLUDE_PATH MATCHES "-NOTFOUND$"))
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
if(CURSES_INCLUDE_PATH AND NOT CURSES_INCLUDE_PATH MATCHES "-NOTFOUND$")
  if(CURSES_INCLUDE_PATH MATCHES "/ncursesw$")
    get_filename_component(_cktools_curses_parent "${CURSES_INCLUDE_PATH}" DIRECTORY)
    set(CURSES_INCLUDE_PATH "${_cktools_curses_parent}" CACHE PATH "" FORCE)
    set(CURSES_INCLUDE_DIR "${_cktools_curses_parent}" CACHE PATH "" FORCE)
    set(CURSES_INCLUDE_DIRS "${_cktools_curses_parent}" CACHE PATH "" FORCE)
  else()
    set(CURSES_INCLUDE_PATH "${CURSES_INCLUDE_PATH}" CACHE PATH "" FORCE)
    if(CURSES_INCLUDE_DIR AND NOT CURSES_INCLUDE_DIR MATCHES "-NOTFOUND$")
      set(CURSES_INCLUDE_DIR "${CURSES_INCLUDE_DIR}" CACHE PATH "" FORCE)
    else()
      set(CURSES_INCLUDE_DIR "${CURSES_INCLUDE_PATH}" CACHE PATH "" FORCE)
    endif()
    if(CURSES_INCLUDE_DIRS AND NOT CURSES_INCLUDE_DIRS MATCHES "-NOTFOUND$")
      set(CURSES_INCLUDE_DIRS "${CURSES_INCLUDE_DIRS}" CACHE STRING "" FORCE)
    else()
      set(CURSES_INCLUDE_DIRS "${CURSES_INCLUDE_DIR}" CACHE STRING "" FORCE)
    endif()
  endif()
endif()

unset(_cktools_curses_parent)
unset(_cktools_curses_search_paths)
unset(_cktools_sdk_path)
unset(_cktools_ncursesw_lib)
unset(_cktools_tinfo_lib)
