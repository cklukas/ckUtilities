ck_add_gtest(ck_find_cli_buffer_tests
  cli_buffer_utils_tests.cpp
  search_backend_tests.cpp
  guided_search_tests.cpp
)

target_include_directories(ck_find_cli_buffer_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/include
    ${PROJECT_SOURCE_DIR}/src/native-core/files
)

target_sources(ck_find_cli_buffer_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/search_backend.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/search_model.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ck-find/src/guided_search.cpp
)

include(${PROJECT_SOURCE_DIR}/cmake/FetchNlohmannJson.cmake)

target_link_libraries(ck_find_cli_buffer_tests
  PRIVATE
    ck_options
    ck_files_platform
    nlohmann_json::nlohmann_json
)
