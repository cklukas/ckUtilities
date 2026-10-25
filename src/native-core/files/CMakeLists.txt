# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
add_library(ck_files_core STATIC destination_directory_assessment.cpp indexed_pane_sources.cpp directory_index_marks.cpp indexed_pane_marks.cpp pane_filter.cpp indexed_pane.cpp directory_index_lookup.cpp directory_index_listing_job.cpp pane_order.cpp directory_index_sort_job.cpp directory_index_store.cpp directory_index_record.cpp inode_flags.cpp location_relation.cpp native_path.cpp native_filesystem.cpp native_name.cpp file_metadata.cpp directory_cursor.cpp windows_directory_record.cpp
  data_channels.cpp windows_stream_records.cpp opened_entry.cpp attributes.cpp security.cpp location_info.cpp storage_info.cpp link_metadata.cpp pane_model.cpp held_child.cpp unicode_scalar.cpp name_mapping.cpp windows_name.cpp child_lookup.cpp name_limits.cpp directory_listing.cpp monotonic_clock.cpp pane_listing_job.cpp directory_entry_probe.cpp pane_refresh_job.cpp object_inventory_job.cpp destination_batch_job.cpp data_layout.cpp pane_acquire_job.cpp source_target_job.cpp windows_parent_record.cpp ancestor_walk_job.cpp directory_access.cpp directory_name_rules.cpp destination_names_job.cpp fscrypt_policy.cpp pane_page.cpp pane_command.cpp fscrypt_key_status.cpp directory_inventory_job.cpp recursive_inventory_job.cpp file_limits.cpp integrity_info.cpp operation_consent.cpp frozen_inventory.cpp preservation_capability_diff.cpp source_capability_projection.cpp operation_plan_assembly.cpp operation_evidence_assembly.cpp destination_conflict_projection.cpp operation_topology_projection.cpp operation_planning_coordinator.cpp operation_review.cpp)
target_include_directories(ck_files_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ck_files_core PUBLIC cxx_std_20)
cktools_strict(ck_files_core)

add_library(ck_files_platform STATIC)
if(WIN32)
  target_sources(ck_files_platform PRIVATE platform/metadata_windows.cpp platform/index_file_windows.cpp platform/home_windows.cpp)
  target_link_libraries(ck_files_platform PRIVATE ntdll advapi32 bcrypt userenv)
else()
  target_sources(ck_files_platform PRIVATE platform/metadata_posix.cpp platform/index_file_posix.cpp platform/home_posix.cpp)
endif()
target_link_libraries(ck_files_platform PUBLIC ck_files_core)
target_compile_features(ck_files_platform PUBLIC cxx_std_20)
cktools_strict(ck_files_platform)
