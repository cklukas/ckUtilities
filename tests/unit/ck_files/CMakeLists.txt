# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(ck_files_core_tests test_files_names.cpp test_files_metadata.cpp test_files_directory.cpp test_files_channels.cpp test_files_attributes.cpp test_files_security.cpp test_files_location.cpp test_files_storage.cpp test_files_links.cpp test_files_pane.cpp test_files_continuity.cpp test_files_name_mapping.cpp test_files_lookup.cpp test_files_name_limits.cpp)
target_link_libraries(ck_files_core_tests PRIVATE ck_files_core ck_vision_cktest_main)
cktools_strict(ck_files_core_tests)
foreach(suite names metadata directory channels attributes security location storage links pane continuity name_mapping lookup name_limits)
  add_test(NAME test_files_${suite} COMMAND ck_files_core_tests --suite test_files_${suite}.cpp)
  set_tests_properties(test_files_${suite} PROPERTIES LABELS "files;unit")
endforeach()

add_executable(ck_files_native_tests test_files_native.cpp test_files_directory_native.cpp test_files_child_native.cpp test_files_channels_native.cpp test_files_attributes_native.cpp test_files_security_native.cpp test_files_location_native.cpp test_files_storage_native.cpp test_files_links_native.cpp test_files_continuity_native.cpp test_files_name_mapping_native.cpp test_files_lookup_native.cpp)
target_link_libraries(ck_files_native_tests PRIVATE ck_files_platform ck_vision_cktest_main)
cktools_strict(ck_files_native_tests)
foreach(suite native directory_native child_native channels_native attributes_native security_native location_native storage_native links_native continuity_native name_mapping_native lookup_native)
  add_test(NAME test_files_${suite} COMMAND ck_files_native_tests --suite test_files_${suite}.cpp)
  set_tests_properties(test_files_${suite} PROPERTIES LABELS "files;native" TIMEOUT 60)
endforeach()
