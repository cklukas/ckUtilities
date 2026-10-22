# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native-host helpers shared by ck-todo and ck-sysmon: the platform terminal,
# clock and filesystem, UTF-8 arguments, the current user, the suite's
# configuration folder, the diagnostic switches, and the About text.
add_library(ck_vision_host INTERFACE)
target_include_directories(ck_vision_host INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ck_vision_host INTERFACE ckvision::cvision)
target_compile_features(ck_vision_host INTERFACE cxx_std_20)
target_compile_definitions(ck_vision_host INTERFACE CK_UTILITIES_VERSION="${PROJECT_VERSION}")

# The cktest entry point shared by the strict apps' test executables.
if(BUILD_TESTING)
  add_library(ck_vision_cktest_main STATIC cktest_main.cpp)
  target_link_libraries(ck_vision_cktest_main PUBLIC ckvision::cvision)
  target_include_directories(ck_vision_cktest_main PUBLIC ${PROJECT_SOURCE_DIR}/tests/support)
  # Native tests create their scratch directories under this explicit,
  # build-owned root; ScratchDirectory never falls back to a host default.
  set(CKTOOLS_TEST_TEMP_ROOT "${PROJECT_BINARY_DIR}/test-scratch" CACHE PATH
      "Explicit root for native test scratch directories")
  if(NOT IS_ABSOLUTE "${CKTOOLS_TEST_TEMP_ROOT}")
    message(FATAL_ERROR "CKTOOLS_TEST_TEMP_ROOT must be absolute")
  endif()
  file(MAKE_DIRECTORY "${CKTOOLS_TEST_TEMP_ROOT}")
  target_compile_definitions(ck_vision_cktest_main PUBLIC
    CKTOOLS_TEST_TEMP_ROOT="${CKTOOLS_TEST_TEMP_ROOT}")
  cktools_strict(ck_vision_cktest_main)
endif()
