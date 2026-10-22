# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ck-todo's teaching-path benchmarks. `todo_bench` prints timings; the
# `--gate` CTest run checks the deterministic work budgets only, never
# wall-clock speed, so it is safe on shared machines.
add_executable(todo_bench bench_todo.cpp)
target_link_libraries(todo_bench PRIVATE ck_vision_todo ckvision::cvision)
target_include_directories(todo_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
cktools_strict(todo_bench)

add_custom_target(todo_bench_run COMMAND todo_bench COMMENT "ck-todo benchmarks")
if(BUILD_TESTING)
  # Sanitized and Debug builds run the same workload far slower.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_CXX_FLAGS MATCHES "sanitize")
    set(_cktools_bench_timeout 600)
  else()
    set(_cktools_bench_timeout 60)
  endif()
  add_test(NAME todo_benchmark_smoke COMMAND todo_bench --gate)
  set_tests_properties(todo_benchmark_smoke PROPERTIES
    LABELS "benchmark;budget;todo" TIMEOUT ${_cktools_bench_timeout})
endif()
