# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)
add_library(ck_vision_files STATIC files_completion_service.cpp files_completion_index.cpp files_location.cpp files_location_text.cpp files_location_draft.cpp files_location_dialog.cpp files_operation_review_dialog.cpp files_operation_planning_service.cpp files_operation_execution_service.cpp files_operation_recovery.cpp files_operation_recovery_cleanup.cpp files_pane_grid.cpp files_pane_service.cpp files_settings.cpp files_settings_service.cpp files_volume_service.cpp files_volume_archive.cpp files_volume_source.cpp files_volume_archive_review_dialog.cpp files_app.cpp files_app_wake.cpp)
target_compile_features(ck_vision_files PUBLIC cxx_std_20)
target_include_directories(ck_vision_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ck_vision_files PUBLIC ck_files_core ck_vision_shell ck_vision_keymap ckvision::cvision Threads::Threads PRIVATE ck_core_fs_native)
cktools_strict(ck_vision_files)

add_executable(ckfiles_ckvision main.cpp)
target_compile_features(ckfiles_ckvision PRIVATE cxx_std_20)
target_link_libraries(ckfiles_ckvision PRIVATE ck_vision_host ck_vision_files ck_vision_keymap)
set_target_properties(ckfiles_ckvision PROPERTIES
  OUTPUT_NAME "ck-files"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
)
install(TARGETS ckfiles_ckvision RUNTIME DESTINATION bin COMPONENT Runtime)

if(BUILD_TESTING)
  set(CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT "${PROJECT_BINARY_DIR}/test-scratch" CACHE PATH
    "Explicit fixture root with native private-file permission enforcement")
  if(NOT IS_ABSOLUTE "${CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT}")
    message(FATAL_ERROR "CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT must be absolute")
  endif()
  file(MAKE_DIRECTORY "${CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT}")
  add_executable(ck_vision_files_tests tests/files_completion_service_tests.cpp tests/files_completion_index_tests.cpp tests/files_location_text_tests.cpp tests/files_location_draft_tests.cpp tests/files_settings_tests.cpp tests/files_settings_service_tests.cpp tests/files_operation_review_dialog_tests.cpp tests/files_operation_planning_service_tests.cpp tests/files_operation_execution_service_tests.cpp tests/files_operation_recovery_tests.cpp tests/files_volume_archive_tests.cpp tests/files_volume_source_tests.cpp tests/files_pane_grid_tests.cpp tests/files_pane_service_tests.cpp tests/files_app_tests.cpp)
  target_link_libraries(ck_vision_files_tests PRIVATE ck_vision_files ck_vision_shell ck_files_platform ck_vision_cktest_main)
  target_compile_definitions(ck_vision_files_tests PRIVATE CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT="${CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT}")
  cktools_strict(ck_vision_files_tests)
  add_test(NAME ck_vision_files_tests COMMAND ck_vision_files_tests)
  set_tests_properties(ck_vision_files_tests PROPERTIES LABELS "files;ckvision-migration" TIMEOUT 60)
  add_executable(ck_vision_files_settings_allocation_bound_tests
    tests/files_settings_allocation_bound_tests.cpp
    tests/files_settings_allocation_bound_allocator.cpp)
  target_link_libraries(ck_vision_files_settings_allocation_bound_tests PRIVATE ck_vision_files)
  cktools_strict(ck_vision_files_settings_allocation_bound_tests)
  add_test(NAME ck_vision_files_settings_allocation_bound_tests COMMAND ck_vision_files_settings_allocation_bound_tests)
  set_tests_properties(ck_vision_files_settings_allocation_bound_tests PROPERTIES LABELS "files;settings;allocation" TIMEOUT 15)
endif()

# Native ownership/finalization contracts use genuine Linux staging providers.
# Other providers are exercised by their capability-specific application tests.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(ck_vision_archive_native_tests
    tests/files_volume_archive_native_tests.cpp tests/archive_fault_allocator.cpp)
  target_link_libraries(ck_vision_archive_native_tests PRIVATE ck_vision_files)
  cktools_strict(ck_vision_archive_native_tests)
  target_compile_options(ck_vision_archive_native_tests PRIVATE -UNDEBUG)
  target_link_options(ck_vision_archive_native_tests PRIVATE
    -Wl,--wrap=ftruncate -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=linkat)
  foreach(archive_case roundtrip occupied hint-replaced cancel-in-write cancel-close
      cancel-close-oom cancel-after-publish close-oom first-enospc-close-oom
      upstream-oom-close post-link-flush service service-legacy-root service-oom no-owner)
    add_test(NAME ck_files_archive_native_${archive_case} COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_volume_archive_native.py
      $<TARGET_FILE:ck_vision_archive_native_tests> ${CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT} 40 ${archive_case})
    set_tests_properties(ck_files_archive_native_${archive_case} PROPERTIES TIMEOUT 45 LABELS "files;archive;native")
  endforeach()
  add_executable(ck_vision_archive_amendment_tests files_volume_archive.cpp
    tests/files_volume_archive_amendment_tests.cpp tests/archive_fault_allocator.cpp)
  target_include_directories(ck_vision_archive_amendment_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ck_vision_archive_amendment_tests PRIVATE ck_core_fs_native)
  target_compile_definitions(ck_vision_archive_amendment_tests PRIVATE CK_FS_ARCHIVE_READBACK_FAILURE_TEST=1)
  cktools_strict(ck_vision_archive_amendment_tests)
  target_compile_options(ck_vision_archive_amendment_tests PRIVATE -UNDEBUG)
  target_link_options(ck_vision_archive_amendment_tests PRIVATE
    -Wl,--wrap=ftruncate -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=close)
  set(_archive_amendment_cases cache cache-oom post-committed-pending
    post-committed-pending-oom post-committed-dtor-pending post-committed-dtor-pending-oom)
  foreach(archive_phase committed close-file complete-journal)
    foreach(archive_suffix "" -fault -oom -dtor -dtor-fault -dtor-oom)
      list(APPEND _archive_amendment_cases post-${archive_phase}${archive_suffix})
    endforeach()
  endforeach()
  foreach(archive_case IN LISTS _archive_amendment_cases)
    add_test(NAME ck_files_archive_amendment_${archive_case} COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_volume_archive_native.py
      $<TARGET_FILE:ck_vision_archive_amendment_tests> ${CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT} 30 ${archive_case} 0)
    set_tests_properties(ck_files_archive_amendment_${archive_case} PROPERTIES TIMEOUT 35 LABELS "files;archive;native;finalization")
  endforeach()
endif()
