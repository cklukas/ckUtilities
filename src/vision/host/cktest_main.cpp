// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The single cktest entry point for ck-todo's and ck-sysmon's test
// executables: every test_*.cpp registers its CK_TEST cases through static
// initialization and must not define its own CKTEST_MAIN. `--suite <file>`
// selects one source file's cases, so CTest lists each suite on its own.
#include "cvision/testing/cktest.hpp"

CKTEST_MAIN
