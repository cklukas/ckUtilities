// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
namespace settings_bound_fixture {
extern bool reject_large_allocation;
extern unsigned rejected;
inline constexpr std::size_t cap = 1024 * 1024;
}
