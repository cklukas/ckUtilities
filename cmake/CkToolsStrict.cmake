# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The zero-warning flag set for targets held to it: ck-todo, ck-sysmon and
# their tests, fuzzers, benchmark and documentation tools, which came from
# ckVision with that guarantee and keep it here. One place defines the flags.

include_guard(GLOBAL)
include(CheckCXXCompilerFlag)

option(CKTOOLS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in strict targets" ON)

# Clang's -Wshadow-all also catches a lambda capture or parameter shadowing an
# outer local, the class GCC's plain -Wshadow reports. Probed, not assumed: a
# flag the compiler does not know protects nothing.
check_cxx_compiler_flag(-Wshadow-all CKTOOLS_HAS_WSHADOW_ALL)
check_cxx_compiler_flag(-Wmissing-designated-field-initializers CKTOOLS_HAS_WMISSING_DESIGNATED)

function(cktools_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    if(CKTOOLS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    # MSVC deprecates std::getenv and std::fopen; both are used correctly here
    # and the "secure" replacements change ownership or return conventions.
    # The macro, not /wd4996, so [[deprecated]] markers still warn.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(CKTOOLS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(CKTOOLS_HAS_WSHADOW_ALL)
      target_compile_options(${target} PRIVATE -Wshadow-all)
    endif()
    if(CKTOOLS_HAS_WMISSING_DESIGNATED)
      # ckVision's descriptor structs default every field so a call site
      # names only what differs; this warning would demand every field.
      target_compile_options(${target} PRIVATE -Wno-missing-designated-field-initializers)
    else()
      # A compiler without the specific flag folds the same diagnostic into
      # -Wmissing-field-initializers, which -Wextra enables (GCC, older Clang).
      target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
    endif()
  endif()
endfunction()
