add_ck_tool(ckchat
  OUTPUT_NAME "ck-chat"
  SOURCES
    src/ck-chat-app.cpp
  LIBRARIES
    ck_app_info
    ckai_core
  INCLUDE_DIRECTORIES
    ${PROJECT_SOURCE_DIR}/include
)
