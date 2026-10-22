# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# libFuzzer targets for ck-todo's workspace codec and ck-sysmon's parsers.
# Opt-in with CKTOOLS_BUILD_FUZZERS=ON: they need an LLVM Clang with the
# libFuzzer runtime, which ordinary and release builds do not.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "CKTOOLS_BUILD_FUZZERS requires an LLVM Clang toolchain with the libFuzzer runtime; "
    "AppleClang is insufficient")
endif()

# Every fuzzer joins this aggregate, so a corpus test can never name an
# executable that a fuzz build does not produce.
add_custom_target(cktools_fuzzers)

function(cktools_add_fuzzer name source corpus)
  add_executable(${name} ${source})
  add_dependencies(cktools_fuzzers ${name})
  target_link_libraries(${name} PRIVATE ${ARGN} ckvision::cvision)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  cktools_strict(${name})
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  if(BUILD_TESTING)
    file(GLOB _corpus CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus}/*")
    add_test(NAME ${name}_corpus
      COMMAND ${name} ${_corpus} -runs=1 -max_len=4096 -timeout=2 -rss_limit_mb=512)
    set_tests_properties(${name}_corpus PROPERTIES LABELS "fuzz-corpus" TIMEOUT 45)
  endif()
endfunction()

cktools_add_fuzzer(fuzz_todo_codec fuzz_todo_codec.cpp todo_codec ck_vision_todo)
cktools_add_fuzzer(fuzz_sysmon fuzz_sysmon.cpp sysmon ck_vision_sysmon)
