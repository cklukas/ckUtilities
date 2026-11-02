# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# libFuzzer targets for ck-todo's workspace codec, ck-sysmon's parsers, and
# the bounded Files operation-journal decoder.
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
  # Product libraries carry ckvision transitively.  Adding it here as well
  # makes the linker see the same static archive twice for those fuzzers.
  # Targets with no product-library dependency add ckvision explicitly below.
  target_link_libraries(${name} PRIVATE ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  cktools_strict(${name})
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  if(BUILD_TESTING)
    add_test(NAME ${name}_corpus
      COMMAND ${CMAKE_COMMAND}
        "-DFUZZER=$<TARGET_FILE:${name}>"
        "-DCORPUS=${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus}"
        "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/corpus-runs/${name}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_corpus_test.cmake")
    set_tests_properties(${name}_corpus PROPERTIES LABELS "fuzz-corpus" TIMEOUT 45)
  endif()
endfunction()

cktools_add_fuzzer(fuzz_todo_codec fuzz_todo_codec.cpp todo_codec ck_vision_todo)
cktools_add_fuzzer(fuzz_sysmon fuzz_sysmon.cpp sysmon ck_vision_sysmon)
cktools_add_fuzzer(fuzz_operation_journal fuzz_operation_journal.cpp operation_journal)
cktools_add_fuzzer(fuzz_files_pane_page fuzz_files_pane_page.cpp files_pane_page ck_files_core)
target_link_libraries(fuzz_operation_journal PRIVATE ckvision::cvision)
target_sources(fuzz_operation_journal PRIVATE
  ${CK_CORE_FS_LIB_SOURCE_DIR}/src/files/operation_journal.cpp)
target_include_directories(fuzz_operation_journal PRIVATE
  ${CK_CORE_FS_LIB_SOURCE_DIR}/src/files)
