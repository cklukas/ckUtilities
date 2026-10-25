// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace ck::files;
namespace fs = std::filesystem;

template <class T> T take(Observation<T> result) {
    if (!result.value())
        throw std::runtime_error("native content fixture: " + result.unavailable_detail()->error.operation);
    return std::move(result).take_value();
}

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

CK_TEST(native_operation_staging_file_is_created_relative_to_held_directory_and_roundtrips_content) {
    const ck::testing::ScratchDirectory scratch("files-operation-content", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto staging = take(directory->create_child_file(name("staging.bin")));
    const std::string text = "held staging content";
    std::vector<std::byte> bytes;
    for (const auto character : text) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    CK_CHECK(take(staging->write_content(0, std::span<const std::byte>(bytes).first(5))) == 5);
    CK_CHECK(take(staging->write_content(5, std::span<const std::byte>(bytes).subspan(5))) == bytes.size() - 5);
    CK_CHECK(take(staging->flush_content()));
    const auto read = take(staging->read_content({bytes.size() + 1}, 0));
    CK_CHECK(read.complete && read.bytes == bytes);
    CK_CHECK(!directory->create_child_file(name("staging.bin")).value());
}

CK_TEST(native_operation_staging_file_rejects_foreign_names_and_cancelled_writes) {
    const ck::testing::ScratchDirectory scratch("files-operation-content-invalid", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
#if defined(_WIN32)
    const auto foreign = NativeName::from_posix("staging.bin");
#else
    const auto foreign = NativeName::from_windows(u"staging.bin");
#endif
    CK_CHECK(!directory->create_child_file(*foreign).value());
    auto staging = take(directory->create_child_file(name("cancelled.bin")));
    std::stop_source stop;
    stop.request_stop();
    const std::array<std::byte, 1> byte{std::byte{1}};
    CK_CHECK(staging->write_content(0, byte, {}, stop.get_token()).state() == ObservationState::Unknown);
}
