# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run a libFuzzer corpus from a disposable build-local copy. libFuzzer writes
# newly discovered units to its corpus argument; never pass a source-tree
# corpus directly to it from a qualification test.

if(NOT DEFINED FUZZER OR NOT DEFINED CORPUS OR NOT DEFINED SCRATCH)
  message(FATAL_ERROR "FUZZER, CORPUS and SCRATCH are required")
endif()

file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}")
if(EXISTS "${CORPUS}")
  file(COPY "${CORPUS}/" DESTINATION "${SCRATCH}")
endif()

execute_process(
  COMMAND "${FUZZER}" "${SCRATCH}" -runs=1 -max_len=4096 -timeout=2 -rss_limit_mb=512
  WORKING_DIRECTORY "${SCRATCH}"
  RESULT_VARIABLE result)

file(REMOVE_RECURSE "${SCRATCH}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "fuzzer corpus contract failed with exit code ${result}")
endif()
