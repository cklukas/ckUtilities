add_library(ckai_core STATIC
  src/config_loader.cpp
  src/llm.cpp
  src/model_manager.cpp
  src/model_manager_controller.cpp
  src/system_prompt_manager.cpp
  src/user_directories.cpp
)

target_include_directories(ckai_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Model downloads use the platform's HTTP client: WinHTTP on Windows (part of
# the operating system), libcurl elsewhere (a CMake package, or pkg-config).
set(CKAI_CORE_CURL_LIBRARIES "")
set(CKAI_CORE_CURL_INCLUDE_DIRS "")
if(WIN32)
  target_sources(ckai_core PRIVATE src/http_download_winhttp.cpp)
  target_link_libraries(ckai_core PRIVATE winhttp)
else()
  target_sources(ckai_core PRIVATE src/http_download_curl.cpp)
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
    nlohmann_json::nlohmann_json
    ck_app_info
  PRIVATE
    ${CKAI_CORE_CURL_LIBRARIES}
)

target_include_directories(ckai_core
  PRIVATE
    ${CKAI_CORE_CURL_INCLUDE_DIRS}
)

target_compile_features(ckai_core PUBLIC cxx_std_20)

install(TARGETS ckai_core
  ARCHIVE DESTINATION lib
  COMPONENT Development)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ck/ai/
  DESTINATION include/ck/ai
  COMPONENT Development
  FILES_MATCHING PATTERN "*.hpp")
