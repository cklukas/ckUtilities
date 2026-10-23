// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "attributes.hpp"
#include <algorithm>
#include <set>
#include <string_view>

namespace ck::files {
AttributeRole attribute_role(const NativeFeatureIdentifier& id, std::uint32_t flags) noexcept {
    if (id.windows_ea_bytes()) return (flags & 0x80U) != 0 ? AttributeRole::InterpretationRequired : AttributeRole::Opaque;
    const auto* raw = id.posix_bytes();
    if (!raw) return AttributeRole::Opaque;
    const std::string_view name(*raw);
    if (name == "com.apple.ResourceFork") return AttributeRole::AdditionalData;
    if (name == "com.apple.decmpfs") return AttributeRole::StorageRepresentation;
    if (name.starts_with("security.") || name == "system.posix_acl_access" || name == "system.posix_acl_default" ||
        name == "com.apple.quarantine" || name == "com.apple.macl") return AttributeRole::Security;
    return AttributeRole::Opaque;
}
namespace detail {
namespace {
template <class T>
Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}
Observation<NativeBytes> read_attribute_bytes(const BufferQuery& query, InventoryLimits limits) {
    if (!limits.valid()) return error<NativeBytes>(ObservationState::Failed, AdapterError::InvalidLimit, "attributes.limits");
    NativeError last_size_error;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto sized = query({});
        if (const auto* failure = std::get_if<UnavailableObservation>(&sized))
            return Observation<NativeBytes>::unavailable(failure->state, failure->error);
        const auto* count = std::get_if<std::size_t>(&sized);
        if (!count) return error<NativeBytes>(ObservationState::Failed, AdapterError::InvalidNativeResult, "attributes.size_result");
        if (*count > limits.max_native_bytes)
            return error<NativeBytes>(ObservationState::Unknown, AdapterError::InventoryLimit, "attributes.value_limit");
        // Size zero must be confirmed with a real nonzero-capacity read. A
        // second size-only query could otherwise grow and fabricate empty data.
        NativeBytes bytes(std::max<std::size_t>(*count, 1));
        const auto read = query(bytes);
        if (const auto* copied = std::get_if<std::size_t>(&read)) {
            if (*copied > bytes.size())
                return error<NativeBytes>(ObservationState::Failed, AdapterError::InvalidNativeResult, "attributes.read_count");
            bytes.resize(*copied);
            return Observation<NativeBytes>::available(std::move(bytes));
        }
        if (const auto* failure = std::get_if<UnavailableObservation>(&read))
            return Observation<NativeBytes>::unavailable(failure->state, failure->error);
        last_size_error = std::get<BufferTooSmall>(read).error;
    }
    return Observation<NativeBytes>::unavailable(ObservationState::Changed, std::move(last_size_error));
}
Observation<std::vector<NativeFeatureIdentifier>> decode_posix_attribute_names(std::span<const std::byte> bytes, InventoryLimits limits) {
    using Names = std::vector<NativeFeatureIdentifier>;
    if (!limits.valid()) return error<Names>(ObservationState::Failed, AdapterError::InvalidLimit, "attributes.limits");
    if (bytes.size() > limits.max_native_bytes) return error<Names>(ObservationState::Unknown, AdapterError::InventoryLimit, "attributes.name_limit");
    Names result;
    std::set<std::string> seen;
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(position);
        const auto end = std::find(first, bytes.end(), std::byte{});
        if (end == bytes.end() || end == first)
            return error<Names>(ObservationState::Failed, AdapterError::InvalidNativeResult, "attributes.name_record");
        if (result.size() == limits.max_records)
            return error<Names>(ObservationState::Unknown, AdapterError::InventoryLimit, "attributes.record_limit");
        std::string spelling;
        for (auto at = first; at != end; ++at) spelling.push_back(static_cast<char>(std::to_integer<unsigned char>(*at)));
        auto id = NativeFeatureIdentifier::from_posix(spelling);
        if (!id || !seen.insert(spelling).second)
            return error<Names>(ObservationState::Failed, AdapterError::InvalidNativeResult, "attributes.identifier");
        result.push_back(std::move(*id));
        position += static_cast<std::size_t>(end - first) + 1;
    }
    return Observation<Names>::available(std::move(result));
}
Observation<WindowsEaRecord> decode_windows_ea_record(std::span<const std::byte> bytes) {
    const auto invalid = [] { return error<WindowsEaRecord>(ObservationState::Failed, AdapterError::InvalidNativeResult, "attributes.ea_record"); };
    if (bytes.size() < 9) return invalid();
    for (std::size_t at = 0; at < 4; ++at) if (bytes[at] != std::byte{}) return invalid();
    const auto name_length = std::to_integer<std::size_t>(bytes[5]);
    const auto value_length = std::to_integer<std::size_t>(bytes[6]) | (std::to_integer<std::size_t>(bytes[7]) << 8U);
    if (name_length == 0 || name_length > bytes.size() - 9 || bytes[8 + name_length] != std::byte{} ||
        value_length > bytes.size() - 9 - name_length) return invalid();
    const auto record_size = 9 + name_length + value_length;
    // Only the terminal record's optional DWORD alignment padding may follow
    // its value. Never accept an unexplained trailing record or byte prefix.
    if (bytes.size() != record_size && bytes.size() != ((record_size + 3) / 4) * 4) return invalid();
    std::string name;
    for (std::size_t at = 0; at < name_length; ++at) name.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[8 + at])));
    auto id = NativeFeatureIdentifier::from_windows_ea(std::move(name));
    if (!id) return invalid();
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(9 + name_length);
    return Observation<WindowsEaRecord>::available({std::move(*id), std::to_integer<std::uint32_t>(bytes[4]),
        NativeBytes(begin, begin + static_cast<std::ptrdiff_t>(value_length))});
}
} // namespace detail
} // namespace ck::files
