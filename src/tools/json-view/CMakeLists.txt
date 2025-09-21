set(CURSES_NEED_WIDE TRUE)
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

add_library(ck_json_view_core STATIC
  src/json_view_core.cpp
)

target_include_directories(ck_json_view_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ck_json_view_core PUBLIC cxx_std_20)

install(TARGETS ck_json_view_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

add_ck_tool(ckjsonview
  OUTPUT_NAME "ck-json-view"
  SOURCES
    src/json-view-app.cpp
  LIBRARIES
    ck_json_view_core
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    JSON_VIEW_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(ck_json_view_core PRIVATE ${CURSES_LIBRARIES})
