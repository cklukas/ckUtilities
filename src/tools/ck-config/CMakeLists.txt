include(FindCursesWithFallback)

include(FetchTurboVision)
cktools_ensure_tvision()

if(TARGET tvision)
  target_include_directories(tvision PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_ck_tool(ckconfig
  OUTPUT_NAME "ck-config"
  SOURCES
    src/config-app.cpp
  LIBRARIES
    ck_app_info
    ck_hotkeys
    ck_options
    ck_du_core
    ck_chat_options
    ck_ui
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-du/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-chat/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_CONFIG_VERSION="${PROJECT_VERSION}"
)
