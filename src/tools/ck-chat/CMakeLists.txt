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
    src/ui/prompt_dialog.cpp
    src/clipboard.cpp
  LIBRARIES
    ck_app_info
    ckai_core
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/lib/ckai_core/src
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_CHAT_VERSION="${PROJECT_VERSION}"
)
