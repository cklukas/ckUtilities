ck_add_gtest(ck_json_view_core_tests
  json_view_core_tests.cpp
)

target_link_libraries(ck_json_view_core_tests
  PRIVATE
    ck_json_view_core
)

target_include_directories(ck_json_view_core_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/json-view/include
)
