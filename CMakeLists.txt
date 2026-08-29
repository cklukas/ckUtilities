cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VersionFromGit)
include(CkVisionPackage)
include(CheckUiBoundaries)
set(CKTOOLS_VERSION "0.1.0" CACHE STRING "ck-utilities version" FORCE)
message(STATUS "ck-utilities version: ${CKTOOLS_VERSION}")

if(APPLE)
  project(cktools VERSION "${CKTOOLS_VERSION}" LANGUAGES CXX OBJCXX)
else()
  project(cktools VERSION "${CKTOOLS_VERSION}" LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Installed executables must locate the shared llama backend from the same
# prefix. The relative location is valid for both an installed binary and a
# dependency loaded from lib/, and keeps the release artifact self-contained.
if(APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
elseif(UNIX)
  set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()

option(CKTOOLS_ENABLE_CKVISION_MIGRATION
       "Build the ckVision-native migration targets"
       OFF)

option(CKTOOLS_CKVISION_CUTOVER
       "Build only the ckVision-native product layout under production names"
       OFF)

if(CKTOOLS_CKVISION_CUTOVER)
  set(CKTOOLS_ENABLE_CKVISION_MIGRATION ON CACHE BOOL
      "Build the ckVision-native migration targets" FORCE)
endif()

if(CKTOOLS_ENABLE_CKVISION_MIGRATION)
  cktools_require_ckvision()
endif()

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

cktools_add_ui_boundary_check()
cktools_add_ckvision_package_verification()
cktools_add_ckvision_install_verification()
cktools_add_ckvision_cutover_verification()

if(CKTOOLS_CKVISION_CUTOVER)
  include(AddCkTool)
  add_subdirectory(src/common)
  add_subdirectory(src/native-core)
  add_subdirectory(src/vision)
  add_subdirectory(src/tools/ck-win-installer)
else()
  add_subdirectory(src/tools)
  add_subdirectory(src/common)

  if(CKTOOLS_ENABLE_CKVISION_MIGRATION)
    add_subdirectory(src/vision)
  endif()
endif()

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
