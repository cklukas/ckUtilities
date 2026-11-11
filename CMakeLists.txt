cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VersionFromGit)
include(CkVisionPackage)
include(CheckUiBoundaries)
set(CKTOOLS_VERSION "0.2.0" CACHE STRING "ck-utilities version" FORCE)
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
  # Executables live in bin/ while shared libraries can depend on siblings in
  # lib/ or lib/<multiarch>/. Keep both the loader's own directory and the
  # executable-relative library locations in the installed runtime path.
  set(_cktools_runtime_rpath "$ORIGIN" "$ORIGIN/../lib")
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
  # Declare the test root before any subdirectory embeds it in a C++ literal.
  set(CKTOOLS_TEST_TEMP_ROOT "${PROJECT_BINARY_DIR}/test-scratch" CACHE PATH
      "Explicit root for native test scratch directories")
  cmake_path(SET CKTOOLS_TEST_TEMP_ROOT "${CKTOOLS_TEST_TEMP_ROOT}")
  if(NOT IS_ABSOLUTE "${CKTOOLS_TEST_TEMP_ROOT}")
    message(FATAL_ERROR "CKTOOLS_TEST_TEMP_ROOT must be absolute")
  endif()
  file(MAKE_DIRECTORY "${CKTOOLS_TEST_TEMP_ROOT}")
endif()

cktools_add_ui_boundary_check()
cktools_add_ckvision_package_verification()
cktools_add_ckvision_install_verification()
cktools_add_ckvision_cutover_verification()

include(CkToolsStrict)

add_subdirectory(src/common)
set(CK_CORE_FS_LIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ck-core-fs-lib" CACHE PATH
  "Source checkout of the headless ck-core-fs-lib backend")
if(NOT EXISTS "${CK_CORE_FS_LIB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "ck-core-fs-lib is required beside ckUtilities; set CK_CORE_FS_LIB_SOURCE_DIR to its checkout")
endif()
add_subdirectory("${CK_CORE_FS_LIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ck-core-fs-lib")
add_subdirectory(src/native-core)
add_subdirectory(src/vision)
add_subdirectory(tools/docgen)
add_subdirectory(benchmarks)

option(CKTOOLS_BUILD_FUZZERS "Build the libFuzzer targets (LLVM Clang only)" OFF)
if(CKTOOLS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
  add_test(NAME ck_tool_documentation_contract
    COMMAND "${CMAKE_COMMAND}"
      -DCKTOOLS_SOURCE_ROOT:PATH=${CMAKE_SOURCE_DIR}
      -P ${CMAKE_SOURCE_DIR}/cmake/VerifyToolDocumentation.cmake)
  set_tests_properties(ck_tool_documentation_contract PROPERTIES LABELS "documentation;release")
endif()

install(FILES README.md DESTINATION share/doc/ck-utilities COMPONENT Runtime)
install(DIRECTORY docs/tools/ DESTINATION share/doc/ck-utilities/tools COMPONENT Runtime)
# Utility guides refer to ../generated/screenshots/<name>.svg. Preserve that
# relative layout in installed packages so the shipped documentation links do
# not silently break after installation.
install(DIRECTORY docs/generated/screenshots/ DESTINATION share/doc/ck-utilities/generated/screenshots COMPONENT Runtime)
install(FILES LICENSE DESTINATION share/licenses/ck-utilities COMPONENT Runtime)
install(FILES configs/ckai.example.toml DESTINATION share/ck-utilities/configs COMPONENT Runtime)

include("${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackConfig.cmake")
cktools_add_ckvision_archive_verification()
cktools_add_ckvision_terminal_verification()
cktools_add_ckvision_rehearsal_verification()

include(CkToolsWindowsManifest)
cktools_embed_windows_manifests()
