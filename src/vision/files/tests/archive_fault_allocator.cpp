// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_fault_allocator.hpp"
#include <cstdlib>
#include <new>
namespace archive_test {
thread_local bool deny_alloc=false;
std::atomic<unsigned> denied_allocations{0};
}
void* operator new(std::size_t n){
    if(archive_test::deny_alloc){archive_test::denied_allocations.fetch_add(1,std::memory_order_relaxed);throw std::bad_alloc();}
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
