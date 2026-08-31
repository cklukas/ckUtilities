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
  set(_cktools_runtime_rpath "$ORIGIN/../lib")
  # Third-party shared libraries use CMake's multi-architecture library
  # directory on Debian-family systems (for example lib/x86_64-linux-gnu).
  # Retain the conventional lib/ lookup and include that directory when the
  # toolchain defines one, so installed executables remain self-contained.
  if(CMAKE_LIBRARY_ARCHITECTURE)
    list(APPEND _cktools_runtime_rpath "$ORIGIN/../lib/${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()
  set(CMAKE_INSTALL_RPATH "${_cktools_runtime_rpath}")
endif()

# ckUtilities is a ckVision product.  A source checkout is deliberately not a
# dependency: every build, test, and package consumes the installed CMake SDK.
cktools_require_ckvision()

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

add_subdirectory(src/common)
add_subdirectory(src/native-core)
add_subdirectory(src/vision)
add_subdirectory(tools/docgen)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(FILES README.md DESTINATION share/doc/ck-utilities)
install(FILES LICENSE DESTINATION share/licenses/ck-utilities)
install(FILES configs/ckai.example.toml DESTINATION share/ck-utilities/configs)

include("${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackConfig.cmake")
cktools_add_ckvision_archive_verification()
cktools_add_ckvision_terminal_verification()
cktools_add_ckvision_rehearsal_verification()
