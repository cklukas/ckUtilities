ck_add_gtest(ck_edit_markdown_tests
  markdown_parser_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/src/markdown_parser.cpp
)

target_include_directories(ck_edit_markdown_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-edit/include
)
