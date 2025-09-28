cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VersionFromGit)
set(CKTOOLS_VERSION "0.1.0" CACHE STRING "ck-utilities version" FORCE)
message(STATUS "ck-utilities version: ${CKTOOLS_VERSION}")

project(cktools VERSION "${CKTOOLS_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add llama.cpp integration
option(CKAI_BACKEND_LLAMA "Enable llama.cpp backend" ON)
if(CKAI_BACKEND_LLAMA)
    include(cmake/FetchLlama.cmake)
else()
    message(FATAL_ERROR "CKAI_BACKEND_LLAMA is required for AI tools")
endif()
add_subdirectory(lib/ckai_core)

include(CTest)
if(BUILD_TESTING)
  enable_testing()
endif()

add_subdirectory(src/tools)
add_subdirectory(src/common)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(FILES README.md DESTINATION share/doc/ck-utilities)
install(FILES LICENSE DESTINATION share/licenses/ck-utilities)
install(FILES configs/ckai.example.toml DESTINATION share/ck-utilities/configs)

if(APPLE)
  install(PROGRAMS packaging/macos/install.sh DESTINATION share/ck-utilities)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackConfig.cmake")
