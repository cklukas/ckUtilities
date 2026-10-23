// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_channels.hpp"
#include "opened_entry.hpp"
#include "windows_stream_records.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include "cvision/testing/cktest.hpp"

namespace {
using namespace ck::files;
void number(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        bytes.at(at + index) = static_cast<std::byte>(value & 0xFFU);
        value >>= 8U;
    }
}
std::vector<std::byte> record(std::u16string_view name, std::uint64_t size = 0, std::uint64_t allocated = 0) {
    std::vector<std::byte> bytes(24 + name.size() * 2);
    number(bytes, 4, name.size() * 2, 4);
    number(bytes, 8, size, 8);
    number(bytes, 16, allocated, 8);
    for (std::size_t index = 0; index < name.size(); ++index) number(bytes, 24 + index * 2, name[index], 2);
    return bytes;
}
std::vector<std::byte> pair(std::u16string_view first, std::u16string_view second) {
    auto bytes = record(first, 42, 4096);
    bytes.resize((bytes.size() + 7) / 8 * 8);
    number(bytes, 0, bytes.size(), 4);
    const auto tail = record(second);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}
class BasicOnlyEntry final : public OpenedEntry {
    Observation<BasicMetadata> inspect_basic() const override { return Observation<BasicMetadata>::available({}); }
};
}

CK_TEST(feature_identifiers_preserve_their_own_namespace_without_path_validation) {
    const std::string raw = std::string("user/a\\b\n") + static_cast<char>(0xFF);
    const auto posix = NativeFeatureIdentifier::from_posix(raw);
    CK_CHECK(posix && *posix->posix_bytes() == raw && !posix->windows_units());
    CK_CHECK(!NativeName::from_posix(raw));
    CK_CHECK(posix->display_name() == "user/a\\\\b\\u{000A}\\xFF");
    std::u16string units = u":a/b\\c:";
    units.push_back(static_cast<char16_t>(0xD800));
    const auto windows = NativeFeatureIdentifier::from_windows(units);
    CK_CHECK(windows && *windows->windows_units() == units && !windows->posix_bytes());
    CK_CHECK(windows->display_name() == ":a/b\\\\c:\\u{D800}");
    CK_CHECK(!NativeFeatureIdentifier::from_posix(""));
    CK_CHECK(!NativeFeatureIdentifier::from_windows(u""));
    CK_CHECK(!NativeFeatureIdentifier::from_posix(std::string("a\0b", 3)));
    CK_CHECK(!NativeFeatureIdentifier::from_windows(std::u16string(u"a\0b", 3)));
}

CK_TEST(channel_coverage_and_unimplemented_queries_never_assert_absence) {
    const DataChannelCatalog unknown;
    CK_CHECK(unknown.coverage == DataChannelCoverage::Unknown && unknown.owner.state() == ObservationState::Unknown);
    BasicOnlyEntry entry;
    const auto unsupported = entry.inspect_data_channels();
    CK_CHECK(unsupported.state() == ObservationState::Unsupported && !unsupported.value());
    CK_CHECK(unsupported.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));
    for (auto invalid : {InventoryLimits{0, 1}, InventoryLimits{1, 0},
         InventoryLimits{InventoryLimits::hard_native_bytes + 1, 1}, InventoryLimits{1, InventoryLimits::hard_records + 1}}) {
        CK_CHECK(!invalid.valid());
        CK_CHECK(entry.inspect_data_channels(invalid).state() == ObservationState::Failed);
    }
}

CK_TEST(windows_stream_records_keep_main_empty_additional_and_opaque_channels_distinct) {
    const auto bytes = pair(u"::$DATA", u":empty:$DATA");
    const auto decoded = detail::decode_windows_stream_records(bytes);
    CK_CHECK(decoded.value() && decoded.value()->channels.size() == 2);
    const auto& catalog = *decoded.value();
    CK_CHECK(catalog.coverage == DataChannelCoverage::WindowsFileStreamInfo);
    CK_CHECK(catalog.owner.state() == ObservationState::Unknown);
    CK_CHECK(catalog.channels[0].kind == DataChannelKind::MainData);
    CK_CHECK(*catalog.channels[0].logical_size.value() == 42 && *catalog.channels[0].allocated_size.value() == 4096);
    CK_CHECK(catalog.channels[1].kind == DataChannelKind::NamedData && *catalog.channels[1].logical_size.value() == 0);
    const auto opaque = detail::decode_windows_stream_records(record(u":native:$UNRECOGNIZED"));
    CK_CHECK(opaque.value() && opaque.value()->channels[0].kind == DataChannelKind::Opaque);
    std::u16string units = u":native-";
    units.push_back(static_cast<char16_t>(0xD800));
    units += u":$DATA";
    const auto native = detail::decode_windows_stream_records(record(units));
    CK_CHECK(native.value() && *native.value()->channels[0].identifier.windows_units() == units);
}

CK_TEST(windows_stream_records_reject_truncation_bad_offsets_and_duplicate_identifiers) {
    const auto valid = record(u"::$DATA");
    for (std::size_t length = 0; length < valid.size(); ++length)
        CK_CHECK(!detail::decode_windows_stream_records(std::span(valid).first(length)).value());
    for (std::uint32_t bad : {0U, 1U, 0xFFFFFFFFU}) {
        auto bytes = valid;
        number(bytes, 4, bad, 4);
        CK_CHECK(!detail::decode_windows_stream_records(bytes).value());
    }
    for (std::uint32_t bad : {1U, 8U, 24U, 40U, 0xFFFFFFF8U}) {
        auto bytes = valid;
        number(bytes, 0, bad, 4);
        CK_CHECK(!detail::decode_windows_stream_records(bytes).value());
    }
    CK_CHECK(!detail::decode_windows_stream_records(pair(u":same:$DATA", u":same:$DATA")).value());
    CK_CHECK(!detail::decode_windows_stream_records(record(std::u16string(u":a\0b:$DATA", 10))).value());
}

CK_TEST(windows_stream_sizes_preserve_signed_native_range_and_reject_negative_results) {
    const auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto valid = detail::decode_windows_stream_records(record(u"::$DATA", max, max));
    CK_CHECK(valid.value() && *valid.value()->channels[0].logical_size.value() == max);
    CK_CHECK(!detail::decode_windows_stream_records(record(u"::$DATA", max + 1, 0)).value());
    CK_CHECK(!detail::decode_windows_stream_records(record(u"::$DATA", 0, max + 1)).value());
}

CK_TEST(stream_inventory_limits_return_unknown_without_a_falsely_complete_prefix) {
    const auto bytes = pair(u"::$DATA", u":other:$DATA");
    for (auto limits : {InventoryLimits{bytes.size() - 1, 2}, InventoryLimits{bytes.size(), 1}}) {
        const auto result = detail::decode_windows_stream_records(bytes, limits);
        CK_CHECK(result.state() == ObservationState::Unknown && !result.value());
        CK_CHECK(result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
    }
    CK_CHECK(detail::decode_windows_stream_records(bytes, {bytes.size(), 2}).value() != nullptr);
}
