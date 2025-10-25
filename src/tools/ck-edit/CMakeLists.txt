include(FindCursesWithFallback)

include(FetchTurboVision)
cktools_ensure_tvision()

if(TARGET tvision)
  target_include_directories(tvision PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_library(ck_edit_core STATIC
  src/markdown_parser.cpp
  src/markdown_editor.cpp
  src/markdown_file_editor.cpp
)

target_include_directories(ck_edit_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ck_edit_core PUBLIC cxx_std_20)

target_link_libraries(ck_edit_core
  PUBLIC
    ck_app_info
    tvision::tvision
    ${CURSES_LIBRARIES}
)

install(TARGETS ck_edit_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

add_ck_tool(ckedit
  OUTPUT_NAME "ck-edit"
  SOURCES
    src/ck-edit-app.cpp
  LIBRARIES
    ck_edit_core
    ck_hotkeys
    tvision::tvision
    ${CURSES_LIBRARIES}
  INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include
    ${CURSES_INCLUDE_DIRS}
  DEFINITIONS
    CK_EDIT_VERSION="${PROJECT_VERSION}"
)
