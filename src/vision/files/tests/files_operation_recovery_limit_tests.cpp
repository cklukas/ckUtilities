// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"
#include "ck/file_io.hpp"
#include "scratch_directory.hpp"
#include <cvision/testing/cktest.hpp>
#include <stdexcept>

namespace {
ck::files::NativeAbsolutePath private_scan_root(const ck::testing::ScratchDirectory &scratch) {
    auto root = ck::files::NativeAbsolutePath::from_host(scratch.path());
    if (!root.value()) throw std::runtime_error("invalid recovery-limit fixture address");
    auto prepared = ck::files::prepare_native_operation_journal_root(*root.value());
    if (!prepared.value() || !*prepared.value())
        throw std::runtime_error("cannot prepare native recovery-limit fixture");
    return std::move(root).take_value();
}
}

CK_TEST(files_operation_recovery_scan_bounds_unrelated_directory_batches) {
    const ck::testing::ScratchDirectory scratch("files-recovery-batch-budget", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = private_scan_root(scratch);
    for (const auto *name : {"unrelated-a", "unrelated-b", "unrelated-c"})
        CK_CHECK(ck::io::write_file(scratch.path() / name, "unchanged"));
    ck::vision::FilesOperationRecoveryLimits limits;
    limits.max_journals = 1;
    limits.max_scan_batches = 1;
    auto limited = ck::vision::discover_operation_recovery(root, limits);
    CK_CHECK(!limited.value() && limited.unavailable_detail());
    if (limited.unavailable_detail()) {
        CK_CHECK(limited.state() == ck::files::ObservationState::Unknown);
        CK_CHECK(limited.unavailable_detail()->error.operation == "operation_recovery.scan_batches");
    }
    limits.max_scan_batches = 128;
    auto complete = ck::vision::discover_operation_recovery(root, limits);
    CK_CHECK(complete.value() && complete.value()->journals.empty() && !complete.value()->requires_reconciliation);
    for (const auto *name : {"unrelated-a", "unrelated-b", "unrelated-c"}) {
        std::string contents;
        CK_CHECK(ck::io::read_file(scratch.path() / name, contents, 32));
        CK_CHECK(contents == "unchanged");
    }
}

CK_TEST(files_operation_recovery_rejects_invalid_scan_budget_before_discovery) {
    const ck::testing::ScratchDirectory scratch("files-recovery-invalid-budget", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto root = ck::files::NativeAbsolutePath::from_host(scratch.path());
    CK_CHECK(root.value());
    if (!root.value()) return;
    for (const auto budget : {std::size_t{0}, std::size_t{4097}}) {
        ck::vision::FilesOperationRecoveryLimits limits;
        limits.max_scan_batches = budget;
        auto result = ck::vision::discover_operation_recovery(*root.value(), limits);
        CK_CHECK(!result.value() && result.unavailable_detail());
        if (result.unavailable_detail())
            CK_CHECK(result.unavailable_detail()->error.operation == "operation_recovery.scan_limits");
    }
}
