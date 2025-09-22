ck_add_gtest(ck_du_core_tests
  disk_usage_tests.cpp
)

target_link_libraries(ck_du_core_tests
  PRIVATE
    ck_du_core
    ck_options
)

target_include_directories(ck_du_core_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-du/include
)
