// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_recovery_worker_oom_fixture.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>
#if defined(_WIN32)
#include <malloc.h>
#endif
namespace recovery_worker_oom {
namespace {
const auto owner = std::this_thread::get_id();
std::atomic<bool> reject{false};
std::atomic<unsigned> rejected{0};
std::atomic<std::terminate_handler> worker_handler{nullptr};
std::atomic<unsigned> verified_handlers{0};
static_assert(std::atomic<std::terminate_handler>::is_always_lock_free);
}
bool configure_worker_terminate_handler(std::terminate_handler handler) noexcept {
    if (!handler || std::this_thread::get_id() != owner || reject.load(std::memory_order_acquire)) return false;
    worker_handler.store(handler, std::memory_order_release);
    return true;
}
unsigned verified_worker_handlers() noexcept { return verified_handlers.load(std::memory_order_relaxed); }
void reject_worker_allocations(bool enabled) noexcept { reject.store(enabled, std::memory_order_release); }
unsigned rejected_allocations() noexcept { return rejected.load(std::memory_order_relaxed); }
void before_allocation() {
    if (reject.load(std::memory_order_acquire) && std::this_thread::get_id() != owner) {
        const auto handler = worker_handler.load(std::memory_order_acquire);
        if (!handler) std::_Exit(87); // Fixture configuration failed, never accept as exit86.
#if defined(_WIN32)
        // MSVC keeps terminate handlers per thread. Configure only this injected
        // worker before throwing; allocation rejection remains active throughout.
        std::set_terminate(handler);
#endif
        if (std::get_terminate() != handler) std::_Exit(87);
        verified_handlers.fetch_add(1, std::memory_order_relaxed);
        rejected.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
}
}
void *operator new(std::size_t n) { recovery_worker_oom::before_allocation(); if (auto *p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t n, std::align_val_t alignment) {
    recovery_worker_oom::before_allocation();
#if defined(_WIN32)
    if (auto *p = _aligned_malloc(n ? n : 1, static_cast<std::size_t>(alignment))) return p;
#else
    void *p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(alignment), n ? n : 1) == 0) return p;
#endif
    throw std::bad_alloc();
}
void *operator new[](std::size_t n, std::align_val_t alignment) { return ::operator new(n, alignment); }
void operator delete(void *p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void *p, std::align_val_t alignment) noexcept { ::operator delete(p, alignment); }
void operator delete(void *p, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(p, alignment); }
void operator delete[](void *p, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(p, alignment); }
