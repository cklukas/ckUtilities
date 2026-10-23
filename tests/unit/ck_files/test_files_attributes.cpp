// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "attributes.hpp"
#include "directory_cursor.hpp"
#include <string>
#include <vector>
#include "cvision/testing/cktest.hpp"

namespace {
using namespace ck::files;
NativeBytes bytes(std::string_view text) {
    NativeBytes result;
    for (const auto ch : text) result.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    return result;
}
NativeBytes ea_record(std::string_view name, std::string_view value, unsigned char flags = 0) {
    NativeBytes result(9 + name.size() + value.size());
    result[4] = static_cast<std::byte>(flags);
    result[5] = static_cast<std::byte>(name.size());
    result[6] = static_cast<std::byte>(value.size() & 255U);
    result[7] = static_cast<std::byte>(value.size() >> 8U);
    for (std::size_t at = 0; at < name.size(); ++at) result[8 + at] = static_cast<std::byte>(name[at]);
    for (std::size_t at = 0; at < value.size(); ++at) result[9 + name.size() + at] = static_cast<std::byte>(value[at]);
    return result;
}
class EmptyCursor final : public DirectoryCursor {
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
}

CK_TEST(attribute_namespaces_and_roles_do_not_imply_cross_platform_equivalence) {
    const auto ea = NativeFeatureIdentifier::from_windows_ea(std::string("A") + "\xC3\xA4");
    CK_CHECK(ea && ea->encoding() == NameEncoding::WindowsEaBytes && ea->windows_units() == nullptr && ea->posix_bytes() == nullptr);
    CK_CHECK(ea->display_name() == "A\\xC3\\xA4");
    CK_CHECK(*NativeFeatureIdentifier::from_windows_ea("A") != *NativeFeatureIdentifier::from_posix("A"));
    CK_CHECK(!NativeFeatureIdentifier::from_windows_ea(std::string(256, 'x')));
    CK_CHECK(!NativeFeatureIdentifier::from_windows_ea(std::string("a\0b", 3)));
    CK_CHECK(attribute_role(*NativeFeatureIdentifier::from_posix("com.apple.ResourceFork")) == AttributeRole::AdditionalData);
    CK_CHECK(attribute_role(*NativeFeatureIdentifier::from_posix("security.selinux")) == AttributeRole::Security);
    CK_CHECK(attribute_role(*NativeFeatureIdentifier::from_posix("system.posix_acl_default")) == AttributeRole::Security);
    CK_CHECK(attribute_role(*NativeFeatureIdentifier::from_posix("com.apple.decmpfs")) == AttributeRole::StorageRepresentation);
    CK_CHECK(attribute_role(*NativeFeatureIdentifier::from_windows_ea("CK.NEEDED"), 0x80) == AttributeRole::InterpretationRequired);
    EmptyCursor cursor;
    CK_CHECK(cursor.open_child(*NativeName::from_posix("name"), static_cast<EntryAccess>(255)).state() == ObservationState::Failed);
    CK_CHECK(AttributeCatalog{}.coverage == AttributeCoverage::Unknown);
}

CK_TEST(attribute_buffer_reader_confirms_empty_values_without_a_second_size_only_query) {
    int calls = 0;
    const auto result = detail::read_attribute_bytes([&](std::span<std::byte> buffer) -> detail::BufferReply {
        ++calls;
        if (calls == 1) { CK_CHECK(buffer.empty()); return std::size_t{0}; }
        CK_CHECK(buffer.size() == 1);
        return std::size_t{0};
    }, {});
    CK_CHECK(result.value() && result.value()->empty() && calls == 2);
}

CK_TEST(attribute_buffer_reader_handles_growth_and_rejects_invalid_counts_and_oversize) {
    int calls = 0;
    const auto result = detail::read_attribute_bytes([&](std::span<std::byte> buffer) -> detail::BufferReply {
        ++calls;
        if (calls == 1) return std::size_t{0};
        if (calls == 2) return detail::BufferTooSmall{{ErrorDomain::Posix, 34, "injected.range"}};
        if (buffer.empty()) return std::size_t{2};
        buffer[0] = std::byte{0}; buffer[1] = std::byte{255};
        return std::size_t{2};
    }, {});
    CK_CHECK(result.value() && *result.value() == (NativeBytes{std::byte{0}, std::byte{255}}) && calls == 4);
    calls = 0;
    const auto limited = detail::read_attribute_bytes([&](std::span<std::byte>) -> detail::BufferReply { ++calls; return std::size_t{100}; }, {10, 1});
    CK_CHECK(limited.state() == ObservationState::Unknown && calls == 1);
    const auto malformed = detail::read_attribute_bytes([](std::span<std::byte> buffer) -> detail::BufferReply {
        return buffer.empty() ? std::size_t{1} : std::size_t{2};
    }, {});
    CK_CHECK(malformed.state() == ObservationState::Failed && !malformed.value());
}

CK_TEST(attribute_buffer_reader_never_retries_denial_io_or_disappearance_and_bounds_churn) {
    for (auto state : {ObservationState::Denied, ObservationState::Failed, ObservationState::Gone}) {
        int calls = 0;
        const NativeError error{ErrorDomain::Posix, 13, "injected.failure"};
        const auto result = detail::read_attribute_bytes([&](std::span<std::byte>) -> detail::BufferReply {
            ++calls; return UnavailableObservation{state, error};
        }, {});
        CK_CHECK(result.state() == state && result.unavailable_detail()->error == error && calls == 1);
    }
    int calls = 0;
    const auto churn = detail::read_attribute_bytes([&](std::span<std::byte> buffer) -> detail::BufferReply {
        ++calls;
        if (buffer.empty()) return std::size_t{1};
        return detail::BufferTooSmall{{ErrorDomain::Posix, 34, "injected.churn"}};
    }, {});
    CK_CHECK(churn.state() == ObservationState::Changed && churn.unavailable_detail()->error.code == 34 && calls == 6);
}

CK_TEST(posix_attribute_name_decoder_preserves_bytes_and_refuses_incomplete_lists) {
    const auto native = bytes(std::string("user.one\0user.raw-", 18) + static_cast<char>(0xFF) + '\0');
    const auto parsed = detail::decode_posix_attribute_names(native, {});
    CK_CHECK(parsed.value() && parsed.value()->size() == 2);
    CK_CHECK(parsed.value()->back().posix_bytes()->back() == static_cast<char>(0xFF));
    CK_CHECK(detail::decode_posix_attribute_names({}, {}).value()->empty());
    for (const auto& malformed : {bytes("user.name"), bytes(std::string("user.a\0user.a\0", 14)), NativeBytes{std::byte{0}}})
        CK_CHECK(!detail::decode_posix_attribute_names(malformed, {}).value());
    CK_CHECK(detail::decode_posix_attribute_names(native, {native.size(), 1}).state() == ObservationState::Unknown);
}

CK_TEST(windows_ea_decoder_preserves_binary_flags_and_zero_length_wire_witnesses) {
    const auto source = ea_record("CK.BINARY", std::string("a\0\xFF", 3), 0x80);
    const auto result = detail::decode_windows_ea_record(source);
    CK_CHECK(result.value() && result.value()->flags == 0x80);
    CK_CHECK(*result.value()->identifier.windows_ea_bytes() == "CK.BINARY");
    CK_CHECK(result.value()->value == bytes(std::string("a\0\xFF", 3)));
    CK_CHECK(detail::decode_windows_ea_record(ea_record("ABSENT", "")).value()->value.empty());
    auto padded = source;
    padded.resize(((source.size() + 3) / 4) * 4);
    CK_CHECK(detail::decode_windows_ea_record(padded).value() != nullptr);
    padded.push_back(std::byte{});
    CK_CHECK(detail::decode_windows_ea_record(padded).state() == ObservationState::Failed);
    for (std::size_t length = 0; length < source.size(); ++length)
        CK_CHECK(!detail::decode_windows_ea_record(std::span(source).first(length)).value());
    for (auto at : {std::size_t{0}, std::size_t{5}, std::size_t{6}, std::size_t{7}, std::size_t{17}}) {
        auto bad = source; bad[at] = std::byte{255};
        CK_CHECK(!detail::decode_windows_ea_record(bad).value());
    }
}
