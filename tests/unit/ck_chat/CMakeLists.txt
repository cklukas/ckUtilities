ck_add_gtest(ck_chat_markdown_tests
  chat_markdown_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/src/markdown_parser.cpp
)

target_include_directories(ck_chat_markdown_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-chat/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include
    ${PROJECT_SOURCE_DIR}/src/common
)
