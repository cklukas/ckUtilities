# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(ck_files_core_tests test_files_names.cpp)
target_link_libraries(ck_files_core_tests PRIVATE ck_files_core ck_vision_cktest_main)
cktools_strict(ck_files_core_tests)
add_test(NAME test_files_names COMMAND ck_files_core_tests --suite test_files_names.cpp)
set_tests_properties(test_files_names PROPERTIES LABELS "files;unit")
