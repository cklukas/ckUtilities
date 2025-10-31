ck_add_gtest(ck_find_cli_buffer_tests
  cli_buffer_utils_tests.cpp
  search_backend_tests.cpp
)

target_include_directories(ck_find_cli_buffer_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/include
)

target_sources(ck_find_cli_buffer_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/search_backend.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/search_model.cpp
)

include(${PROJECT_SOURCE_DIR}/cmake/FetchNlohmannJson.cmake)

target_link_libraries(ck_find_cli_buffer_tests
  PRIVATE
    ck_options
    nlohmann_json::nlohmann_json
)
