set(CURSES_NEED_WIDE TRUE)

if(APPLE)
    find_path(CURSES_INCLUDE_PATH
        NAMES ncurses.h
        PATHS ${CMAKE_OSX_SYSROOT}/usr/include
        NO_DEFAULT_PATH)
endif()

find_package(Curses REQUIRED)

if(CURSES_INCLUDE_PATH MATCHES "/ncursesw$")
  get_filename_component(_curses_parent "${CURSES_INCLUDE_PATH}" DIRECTORY)
  set(CURSES_INCLUDE_PATH "${_curses_parent}" CACHE PATH "" FORCE)
  set(CURSES_INCLUDE_DIR "${_curses_parent}" CACHE PATH "" FORCE)
  set(CURSES_INCLUDE_DIRS "${_curses_parent}" CACHE PATH "" FORCE)
endif()

include(FetchTurboVision)
cktools_ensure_tvision()

if(TARGET tvision)
  target_include_directories(tvision PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_ck_tool(ckutilities
  OUTPUT_NAME "ck-utilities"
  SOURCES
    src/ck-utilities-app.cpp
  LIBRARIES
    ck_app_info
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_UTILITIES_VERSION="${PROJECT_VERSION}"
)

