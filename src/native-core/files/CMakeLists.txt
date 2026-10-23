# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
add_library(ck_files_core STATIC native_name.cpp file_metadata.cpp directory_cursor.cpp windows_directory_record.cpp
  data_channels.cpp windows_stream_records.cpp opened_entry.cpp)
target_include_directories(ck_files_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ck_files_core PUBLIC cxx_std_20)
cktools_strict(ck_files_core)

add_library(ck_files_platform STATIC)
if(WIN32)
  target_sources(ck_files_platform PRIVATE platform/metadata_windows.cpp)
  target_link_libraries(ck_files_platform PRIVATE ntdll)
else()
  target_sources(ck_files_platform PRIVATE platform/metadata_posix.cpp)
endif()
target_link_libraries(ck_files_platform PUBLIC ck_files_core)
target_compile_features(ck_files_platform PUBLIC cxx_std_20)
cktools_strict(ck_files_platform)
