// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_settings_allocation_bound_fixture.hpp"
#include <cstdlib>
#include <new>
namespace settings_bound_fixture {
bool reject_large_allocation = false;
unsigned rejected = 0;
}
using namespace settings_bound_fixture;
// Keep the replacement allocation boundary out of the consumer TU. All hooks
// retain the same compiler, optimization and sanitizer flags as the fixture.
void *operator new(std::size_t size) {
    if (reject_large_allocation && size > cap) { ++rejected; throw std::bad_alloc(); }
    if (auto *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
