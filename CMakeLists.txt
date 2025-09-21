cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VersionFromGit)
cktools_version_from_git(CKTOOLS_VERSION)

project(cktools VERSION "${CKTOOLS_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
  enable_testing()
endif()

add_subdirectory(src/tools)

install(FILES README.md DESTINATION share/doc/ck-utilities)
install(FILES LICENSE DESTINATION share/licenses/ck-utilities)

if(APPLE)
  install(PROGRAMS packaging/macos/install.sh DESTINATION share/ck-utilities)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackConfig.cmake")
