// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <exception>
namespace recovery_worker_oom {
// Configure on the owner before starting any injected worker; never allocates.
bool configure_worker_terminate_handler(std::terminate_handler handler) noexcept;
unsigned verified_worker_handlers() noexcept;
void reject_worker_allocations(bool enabled) noexcept;
unsigned rejected_allocations() noexcept;
}
