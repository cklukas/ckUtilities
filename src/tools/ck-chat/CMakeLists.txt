include(FindCursesWithFallback)

include(FetchTurboVision)
cktools_ensure_tvision()

if(TARGET tvision)
  target_include_directories(tvision PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_library(ck_chat_options STATIC
  src/chat_options.cpp
)
target_include_directories(ck_chat_options
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src/tools/ck-chat/include
)
target_link_libraries(ck_chat_options
  PUBLIC
    ck_options
)

add_ck_tool(ckchat
  OUTPUT_NAME "ck-chat"
  SOURCES
    src/ck-chat-app.cpp
    src/chat_session.cpp
    src/ui/prompt_input_view.cpp
    src/ui/chat_transcript_view.cpp
    src/ui/chat_window.cpp
    src/ui/chat_app.cpp
    src/ui/model_dialog.cpp
    src/ui/model_loading_dialog.cpp
    src/ui/prompt_dialog.cpp
    src/clipboard.cpp
  LIBRARIES
    ck_app_info
    ck_hotkeys
    ck_edit_core
    ckai_core
    ck_chat_options
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/lib/ckai_core/src
    ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_CHAT_VERSION="${PROJECT_VERSION}"
)
