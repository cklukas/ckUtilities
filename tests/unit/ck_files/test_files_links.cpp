// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link_metadata.hpp"
#include "cvision/testing/cktest.hpp"
#include <cstring>

namespace {
using namespace ck::files;
void put(NativeBytes& bytes, std::size_t at, std::uint32_t value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) bytes[at + index] = static_cast<std::byte>((value >> (index * 8U)) & 255U);
}
NativeBytes link_record(bool symlink, const std::u16string& substitute, const std::u16string& print, std::uint32_t flags = 0) {
    const std::size_t prefix = symlink ? 12 : 8;
    const auto data_length = prefix + (substitute.size() + print.size()) * 2;
    NativeBytes result(8 + data_length);
    put(result, 0, symlink ? 0xa000000c : 0xa0000003, 4);
    put(result, 4, static_cast<std::uint32_t>(data_length), 2);
    // Reverse string order is valid: print first, substitute second.
    put(result, 8, static_cast<std::uint32_t>(print.size() * 2), 2);
    put(result, 10, static_cast<std::uint32_t>(substitute.size() * 2), 2);
    put(result, 12, 0, 2); put(result, 14, static_cast<std::uint32_t>(print.size() * 2), 2);
    if (symlink) put(result, 16, flags, 4);
    std::size_t at = 8 + prefix;
    for (const auto* text : {&print, &substitute}) for (const auto unit : *text) { put(result, at, unit, 2); at += 2; }
    return result;
}
}
CK_TEST(posix_link_reader_uses_a_truncation_witness_and_preserves_raw_targets) {
    const std::string target = "../" + std::string(700, 'x') + "\xff\n";
    int calls = 0;
    const auto result = detail::read_posix_link([&](std::span<std::byte> buffer) -> detail::LinkReadReply {
        ++calls;
        const auto size = std::min(buffer.size(), target.size());
        std::memcpy(buffer.data(), target.data(), size);
        return size;
    }, {target.size(), 1});
    CK_CHECK(result.value() && result.value()->target == target && calls == 3);
    const auto truncated = detail::read_posix_link([](std::span<std::byte> buffer) -> detail::LinkReadReply {
        return buffer.size();
    }, {256, 1});
    CK_CHECK(truncated.state() == ObservationState::Unknown && !truncated.value());
}
CK_TEST(posix_link_reader_never_returns_a_prefix_after_failure_or_retries_native_errors) {
    int calls = 0;
    const NativeError error{ErrorDomain::Posix, 13, "injected.readlink"};
    const auto denied = detail::read_posix_link([&](std::span<std::byte>) -> detail::LinkReadReply {
        ++calls; return UnavailableObservation{ObservationState::Denied, error};
    }, {});
    CK_CHECK(denied.state() == ObservationState::Denied && denied.unavailable_detail()->error == error && calls == 1);
    const auto malformed = detail::read_posix_link([](std::span<std::byte> buffer) -> detail::LinkReadReply { return buffer.size() + 1; }, {});
    CK_CHECK(malformed.state() == ObservationState::Failed);
    const auto nul = detail::read_posix_link([](std::span<std::byte> buffer) -> detail::LinkReadReply { buffer[0] = std::byte{}; return std::size_t{1}; }, {});
    CK_CHECK(nul.state() == ObservationState::Failed);
    CK_CHECK(detail::read_posix_link([](std::span<std::byte>) -> detail::LinkReadReply { return std::size_t{0}; }, {0, 1}).state() == ObservationState::Failed);
}
CK_TEST(link_metadata_defaults_do_not_fabricate_an_empty_link) {
    CK_CHECK(std::holds_alternative<std::monostate>(LinkMetadata{}.payload));
    CK_CHECK(LinkMetadata{}.owner.state() == ObservationState::Unknown);
}
CK_TEST(windows_link_records_preserve_counted_native_names_raw_bytes_and_unknown_flags) {
    std::u16string substitute = u"..\\target-"; substitute.push_back(static_cast<char16_t>(0xd800));
    const auto source = link_record(true, substitute, u"display", 0x81);
    const auto parsed = detail::decode_windows_reparse(source, {});
    CK_CHECK(parsed.value() && parsed.value()->kind == WindowsReparseKind::SymbolicLink);
    CK_CHECK(parsed.value()->raw == source && parsed.value()->names->substitute == substitute);
    CK_CHECK(parsed.value()->names->print == u"display" && parsed.value()->names->flags == 0x81);
    const auto mount = detail::decode_windows_reparse(link_record(false, u"\\??\\C:\\target", u""), {});
    CK_CHECK(mount.value() && mount.value()->kind == WindowsReparseKind::MountPoint && mount.value()->names->print.empty());
}
CK_TEST(windows_reparse_decoder_bounds_every_header_offset_length_and_counted_string) {
    const auto source = link_record(true, u"target", u"print");
    for (std::size_t length = 0; length < source.size(); ++length)
        CK_CHECK(detail::decode_windows_reparse(std::span(source).first(length), {}).state() == ObservationState::Failed);
    for (const auto field : {std::size_t{4}, std::size_t{8}, std::size_t{10}, std::size_t{12}, std::size_t{14}}) {
        auto bad = source; put(bad, field, 0xffff, 2);
        CK_CHECK(detail::decode_windows_reparse(bad, {}).state() == ObservationState::Failed);
    }
    auto nul = source; put(nul, 20, 0, 2);
    CK_CHECK(detail::decode_windows_reparse(nul, {}).state() == ObservationState::Failed);
    auto trailing = source; trailing.push_back(std::byte{});
    CK_CHECK(detail::decode_windows_reparse(trailing, {}).state() == ObservationState::Failed);
    CK_CHECK(detail::decode_windows_reparse(source, {source.size() - 1, 1}).state() == ObservationState::Unknown);
}
CK_TEST(opaque_microsoft_and_guid_reparse_data_are_retained_without_link_interpretation) {
    for (const auto microsoft : {true, false}) {
        NativeBytes source((microsoft ? 8 : 24) + 3);
        put(source, 0, microsoft ? 0x80000042 : 0x42, 4); put(source, 4, 3, 2);
        if (!microsoft) source[8] = std::byte{0x91};
        source.back() = std::byte{255};
        const auto parsed = detail::decode_windows_reparse(source, {});
        CK_CHECK(parsed.value() && parsed.value()->raw == source && !parsed.value()->names);
        CK_CHECK(parsed.value()->kind == (microsoft ? WindowsReparseKind::OpaqueMicrosoft : WindowsReparseKind::OpaqueGuid));
        CK_CHECK(parsed.value()->owner_guid.has_value() == !microsoft);
    }
}
