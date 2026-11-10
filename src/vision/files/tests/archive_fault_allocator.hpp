// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
namespace archive_test {
extern thread_local bool deny_alloc;
extern std::atomic<unsigned> denied_allocations;
}
