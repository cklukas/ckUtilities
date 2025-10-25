add_library(ckai_core STATIC
  src/config_loader.cpp
  src/llm.cpp
  src/model_manager.cpp
  src/model_manager_controller.cpp
  src/system_prompt_manager.cpp
)

target_include_directories(ckai_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Find libcurl (prefer CMake package, fallback to pkg-config)
find_package(CURL QUIET)
if(NOT CURL_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CURL REQUIRED libcurl)
endif()

# Find nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  include(${PROJECT_SOURCE_DIR}/cmake/FetchNlohmannJson.cmake)
endif()

target_link_libraries(ckai_core
  PUBLIC
    llama
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    ck_app_info
)

target_include_directories(ckai_core
  PRIVATE
    ${CURL_INCLUDE_DIRS}
)

target_compile_features(ckai_core PUBLIC cxx_std_20)

install(TARGETS ckai_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ck/ai/
  DESTINATION include/ck/ai
  FILES_MATCHING PATTERN "*.hpp"
)
