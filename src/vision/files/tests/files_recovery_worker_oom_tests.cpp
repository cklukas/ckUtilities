// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"
#include "files_recovery_worker_oom_fixture.hpp"
#include "scratch_directory.hpp"
#include "ck/file_io.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <thread>

int main() {
    // The old worker catch throws a second allocation failure. Make that
    // negative control a bounded child exit, without a core dump or retries.
    const std::terminate_handler handler = +[]() noexcept {
        std::fprintf(stderr, "TERMINATE worker_oom rejected=%u handler_verified=%u\n", recovery_worker_oom::rejected_allocations(), recovery_worker_oom::verified_worker_handlers());
        std::_Exit(86);
    };
    std::set_terminate(handler);
    if (!recovery_worker_oom::configure_worker_terminate_handler(handler)) return 10;
    const ck::testing::ScratchDirectory scratch("files-recovery-worker-oom", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto root = ck::files::NativeAbsolutePath::from_host(scratch.path());
    if (!root.value()) return 2;
    if (!ck::io::write_file(scratch.path() / "sentinel", "unchanged")) return 3;
    std::atomic<bool> control{false};
    std::atomic<unsigned> wakes{0};
    recovery_worker_oom::reject_worker_allocations(true);
    std::thread probe([&] {
        try { auto *p = ::operator new(64); ::operator delete(p); }
        catch (const std::bad_alloc &) { control.store(true, std::memory_order_release); }
    });
    probe.join();
    if (!control.load(std::memory_order_acquire) || recovery_worker_oom::verified_worker_handlers() != 1) return 4;
    ck::vision::FilesOperationRecoveryService service([&] { wakes.fetch_add(1, std::memory_order_release); });
    unsigned received = 0;
    for (unsigned round = 0; round != 2; ++round) {
        if (service.request(*root.value()) != ck::vision::FilesOperationRecoverySubmission::Accepted) return 5;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool done = false;
        do {
            if (auto reply = service.take()) {
                if (reply->state != ck::vision::FilesOperationRecoveryState::WorkerFault || reply->error || reply->scan) return 6;
                ++received;
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!done) return 7;
    }
    service.request_stop();
    service.join();
    recovery_worker_oom::reject_worker_allocations(false);
    std::string sentinel;
    if (!ck::io::read_file(scratch.path() / "sentinel", sentinel, 32) || sentinel != "unchanged") return 8;
    if (received != 2 || wakes.load(std::memory_order_acquire) != 2 || recovery_worker_oom::rejected_allocations() < 3) return 9;
    if (recovery_worker_oom::verified_worker_handlers() != recovery_worker_oom::rejected_allocations()) return 11;
    std::puts("PASS persistent worker allocation failure: control, two fault replies, wake, joined shutdown, bytes preserved");
    return 0;
}
