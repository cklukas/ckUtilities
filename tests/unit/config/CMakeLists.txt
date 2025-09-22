ck_add_gtest(ck_config_options_tests
  option_registry_tests.cpp
)

target_link_libraries(ck_config_options_tests
  PRIVATE
    ck_options
)

target_include_directories(ck_config_options_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
