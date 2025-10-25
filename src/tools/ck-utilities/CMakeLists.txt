include(FindCursesWithFallback)

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
    ck_hotkeys
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-utilities/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_UTILITIES_VERSION="${PROJECT_VERSION}"
)
