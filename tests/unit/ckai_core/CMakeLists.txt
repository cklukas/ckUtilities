ck_add_gtest(test_ckai_core_config
  config_loader_tests.cpp
)

target_link_libraries(test_ckai_core_config
  PRIVATE
    ckai_core
)

target_include_directories(test_ckai_core_config PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

ck_add_gtest(test_ckai_core_llm
  llm_tests.cpp
)

target_link_libraries(test_ckai_core_llm
  PRIVATE
    ckai_core
)

target_include_directories(test_ckai_core_llm PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)
