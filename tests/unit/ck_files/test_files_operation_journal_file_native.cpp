// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal_file.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <filesystem>
#include <stdexcept>

namespace {
using namespace ck::files;
namespace fs = std::filesystem;

template <class T> T take(Observation<T> result) {
    if (!result.value())
        throw std::runtime_error("native journal fixture: " + result.unavailable_detail()->error.operation);
    return std::move(result).take_value();
}

NativeAbsolutePath address(const fs::path& path) { return take(NativeAbsolutePath::from_host(path)); }

NativeName name(const char* text) {
#if defined(_WIN32)
    std::u16string value;
    for (const auto* p = text; *p; ++p) value.push_back(static_cast<char16_t>(*p));
    return *NativeName::from_windows(std::move(value));
#else
    return *NativeName::from_posix(text);
#endif
}
}

CK_TEST(operation_journal_file_native_roundtrips_after_close_and_reopen) {
    const ck::testing::ScratchDirectory scratch("files-operation-journal", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto journal_name = name("operation.journal");
    auto first = take(open_native_operation_journal_file(address(scratch.path()), journal_name, true));
    OperationJournalId journal_id;
    journal_id.bytes[0] = std::byte{1};
    auto created = OperationJournal::create(FileOperation::Copy, journal_id, {4, 16, 64});
    if (!created.value()) throw std::runtime_error("journal model fixture failed");
    auto value = std::move(created).take_value();
    CK_CHECK(append_operation_journal_file(*first, *value,
        {1, 0, OperationJournalState::Planned, {std::byte{0x11}}}).value());
    first.reset();
    auto reopened = take(open_native_operation_journal_file(address(scratch.path()), journal_name, false));
    auto loaded = take(load_operation_journal(*reopened, {4, 16, 64}));
    CK_CHECK(loaded.journal && !loaded.tail && loaded.journal->records().size() == 1);
    CK_CHECK(!open_native_operation_journal_file(address(scratch.path()), journal_name, true).value());
}

CK_TEST(operation_journal_file_native_rejects_foreign_name_encoding_and_non_private_root) {
    const ck::testing::ScratchDirectory scratch("files-operation-journal-invalid", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = address(scratch.path());
#if defined(_WIN32)
    const auto foreign = NativeName::from_posix("operation.journal");
#else
    const auto foreign = NativeName::from_windows(u"operation.journal");
#endif
    CK_CHECK(!open_native_operation_journal_file(root, *foreign, true).value());
    const auto missing = address(scratch.path() / "missing");
    CK_CHECK(!open_native_operation_journal_file(missing, name("operation.journal"), true).value());
}
