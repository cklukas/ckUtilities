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

# Find libcurl (prefer a CMake package/target, fallback to pkg-config on Unix)
set(CKAI_CORE_CURL_LIBRARIES "")
set(CKAI_CORE_CURL_INCLUDE_DIRS "")

find_package(CURL CONFIG QUIET)
if(NOT CURL_FOUND)
  find_package(CURL QUIET)
endif()

if(CURL_FOUND)
  if(TARGET CURL::libcurl)
    set(CKAI_CORE_CURL_LIBRARIES CURL::libcurl)
  else()
    set(CKAI_CORE_CURL_LIBRARIES ${CURL_LIBRARIES})
    set(CKAI_CORE_CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIRS})
  endif()
else()
  if(WIN32)
    message(FATAL_ERROR "libcurl not found. Ensure the Windows toolchain provides curl (e.g. via vcpkg).")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURL REQUIRED libcurl)
    set(CKAI_CORE_CURL_LIBRARIES ${CURL_LIBRARIES})
    set(CKAI_CORE_CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIRS})
  endif()
endif()

# Find nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  include(${PROJECT_SOURCE_DIR}/cmake/FetchNlohmannJson.cmake)
endif()

target_link_libraries(ckai_core
  PUBLIC
    llama
    ${CKAI_CORE_CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    ck_app_info
)

target_include_directories(ckai_core
  PRIVATE
    ${CKAI_CORE_CURL_INCLUDE_DIRS}
)

target_compile_features(ckai_core PUBLIC cxx_std_20)

install(TARGETS ckai_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ck/ai/
  DESTINATION include/ck/ai
  FILES_MATCHING PATTERN "*.hpp"
)
