ck_add_gtest(ck_app_info_tests
  app_info_tests.cpp
)

target_link_libraries(ck_app_info_tests
  PRIVATE
    ck_app_info
)

target_include_directories(ck_app_info_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
