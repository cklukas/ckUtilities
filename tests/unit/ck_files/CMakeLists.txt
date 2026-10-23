# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(ck_files_core_tests test_files_names.cpp test_files_metadata.cpp test_files_directory.cpp)
target_link_libraries(ck_files_core_tests PRIVATE ck_files_core ck_vision_cktest_main)
cktools_strict(ck_files_core_tests)
foreach(suite names metadata directory)
  add_test(NAME test_files_${suite} COMMAND ck_files_core_tests --suite test_files_${suite}.cpp)
  set_tests_properties(test_files_${suite} PROPERTIES LABELS "files;unit")
endforeach()

add_executable(ck_files_native_tests test_files_native.cpp test_files_directory_native.cpp test_files_child_native.cpp)
target_link_libraries(ck_files_native_tests PRIVATE ck_files_platform ck_vision_cktest_main)
cktools_strict(ck_files_native_tests)
foreach(suite native directory_native child_native)
  add_test(NAME test_files_${suite} COMMAND ck_files_native_tests --suite test_files_${suite}.cpp)
  set_tests_properties(test_files_${suite} PROPERTIES LABELS "files;native" TIMEOUT 60)
endforeach()
