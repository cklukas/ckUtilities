ck_add_gtest(ck_find_cli_buffer_tests
  cli_buffer_utils_tests.cpp
)

target_include_directories(ck_find_cli_buffer_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/include
)
