// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"

#include "operation_journal_file.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "scratch_directory.hpp"
#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using namespace ck::files;
using namespace ck::vision;

NativeAbsolutePath address(const std::filesystem::path &path) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value())
        throw std::runtime_error("invalid operation recovery fixture address");
    return std::move(value).take_value();
}

OperationJournalId journal_id() {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x91};
    return id;
}

NativeOperationRecoveryEvidence evidence() {
    AdapterScope scope;
    const auto identity = *NativeDurableIdentity::from_live(
        {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(17),
         NativeIdentifier::from_u64(18), {}});
    return {*NativeName::from_posix(".ck-staging"), *NativeName::from_posix("destination"),
            identity, identity, identity, identity};
}
}

CK_TEST(files_operation_recovery_discovery_retains_unfinished_and_malformed_journals) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-discovery");
    const auto root = address(scratch.path());
    CK_CHECK(prepare_native_operation_journal_root(root).value());

    const auto name = *NativeName::from_posix(".ck-copy-journal-0001");
    auto created = OperationJournal::create(FileOperation::Copy, journal_id(), {4, 32, 4096});
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto journal = std::move(created).take_value();
    const auto encoded = encode_native_operation_recovery_evidence(evidence());
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;

    auto file = open_native_operation_journal_file(root, name, true);
    CK_CHECK(file.value());
    if (!file.value()) return;
    CK_CHECK(append_operation_journal_file(**file.value(), *journal,
                                          {1, 0, OperationJournalState::Planned, {}}).value());
    CK_CHECK(append_operation_journal_file(**file.value(), *journal,
                                          {2, 0, OperationJournalState::Prepared, *encoded.value()}).value());

    std::ofstream malformed(scratch.path() / ".ck-move-journal-0002", std::ios::binary);
    malformed << "torn";
    std::filesystem::create_directory(scratch.path() / ".ck-delete-tree-journal-0003");

    const auto scan = discover_operation_recovery(root);
    CK_CHECK(scan.value() && scan.value()->journals.size() == 3 &&
             scan.value()->requires_reconciliation);
    if (!scan.value()) return;
    const auto found = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
                                    [](const auto &item) { return item.display_name == ".ck-copy-journal-0001"; });
    CK_CHECK(found != scan.value()->journals.end() && found->report.has_value());
    if (found != scan.value()->journals.end() && found->report) {
        CK_CHECK(found->report->journal_id == journal_id());
        CK_CHECK(found->report->inspection.report.requires_reconciliation);
        CK_CHECK(found->report->inspection.artifacts.size() == 1);
    }
    const auto malformed_item = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
                                             [](const auto &item) { return item.display_name == ".ck-move-journal-0002"; });
    const auto directory_item = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
                                             [](const auto &item) { return item.display_name == ".ck-delete-tree-journal-0003"; });
    CK_CHECK(malformed_item != scan.value()->journals.end() && malformed_item->error.has_value());
    CK_CHECK(directory_item != scan.value()->journals.end() && directory_item->error.has_value());
}
