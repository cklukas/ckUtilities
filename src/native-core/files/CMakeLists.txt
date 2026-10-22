# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
add_library(ck_files_core STATIC native_name.cpp)
target_include_directories(ck_files_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ck_files_core PUBLIC cxx_std_20)
cktools_strict(ck_files_core)
