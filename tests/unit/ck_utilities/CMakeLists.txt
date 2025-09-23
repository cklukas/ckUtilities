ck_add_gtest(ck_utilities_cli_utils_tests
  cli_utils_tests.cpp
)

target_include_directories(ck_utilities_cli_utils_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-utilities/include
)
