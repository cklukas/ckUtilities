include(FindCursesWithFallback)

include(FetchTurboVision)
cktools_ensure_tvision()

include(${PROJECT_SOURCE_DIR}/cmake/FetchNlohmannJson.cmake)

if(TARGET tvision)
  target_include_directories(tvision PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_ck_tool(ckfind
  OUTPUT_NAME "ck-find"
  SOURCES
    src/ck-find-app.cpp
    src/action_options_dialog.cpp
    src/dialog_utils.cpp
    src/name_path_dialog.cpp
    src/permission_ownership_dialog.cpp
    src/search_backend.cpp
    src/search_dialog.cpp
    src/guided_search.cpp
    src/search_model.cpp
    src/size_filters_dialog.cpp
    src/text_options_dialog.cpp
    src/time_filters_dialog.cpp
    src/traversal_filters_dialog.cpp
    src/type_filters_dialog.cpp
  LIBRARIES
    ck_app_info
    ck_hotkeys
    ck_ui
    nlohmann_json::nlohmann_json
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_FIND_VERSION="${PROJECT_VERSION}"
)
