// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "name_mapping.hpp"
#include "unicode_scalar.hpp"
#include <algorithm>

namespace ck::files {
namespace {
Observation<NativeName> unavailable(ObservationState state, AdapterError code, const char* operation) {
    return Observation<NativeName>::unavailable(state,{ErrorDomain::Adapter,static_cast<std::int64_t>(code),operation});
}
}
Observation<NativeName> map_native_name(const NativeName& source, NameEncoding destination, NameMappingLimits limits) {
    using Result = Observation<NativeName>;
    if (limits.max_native_bytes==0||limits.max_native_bytes>NameMappingLimits::hard_native_bytes)
        return unavailable(ObservationState::Failed,AdapterError::InvalidLimit,"name.mapping_limit");
    if (destination!=NameEncoding::PosixBytes&&destination!=NameEncoding::WindowsUtf16)
        return unavailable(ObservationState::Failed,AdapterError::InvalidIdentifier,"name.destination_encoding");
    const auto budget=limits.max_native_bytes;
    if(!source.posix_bytes()&&!source.windows_units())return unavailable(ObservationState::Failed,AdapterError::InvalidIdentifier,"name.source_component");
    if ((source.posix_bytes()&&source.posix_bytes()->size()>budget)||
        (source.windows_units()&&source.windows_units()->size()>budget/sizeof(char16_t)))
        return unavailable(ObservationState::Unknown,AdapterError::InventoryLimit,"name.input_limit");
    if (!source.is_valid()) return unavailable(ObservationState::Failed,AdapterError::InvalidIdentifier,"name.source_component");
    if (source.encoding()==destination) return Result::available(source);
    if (source.posix_bytes()) {
        const auto& bytes=*source.posix_bytes();std::u16string units;
        units.reserve(std::min(bytes.size(),budget/sizeof(char16_t)));
        for(std::size_t at=0;at<bytes.size();) {
            const auto decoded=detail::decode_utf8_scalar(std::string_view(bytes).substr(at));
            if(decoded.units==0) return unavailable(ObservationState::Unsupported,AdapterError::UnrepresentableName,"name.invalid_utf8");
            const auto encoded=detail::encode_utf16_scalar(decoded.value);
            if(encoded.units==0) return unavailable(ObservationState::Failed,AdapterError::InvalidNativeResult,"name.codec_result");
            if(encoded.units>budget/sizeof(char16_t)-units.size()) return unavailable(ObservationState::Unknown,AdapterError::InventoryLimit,"name.output_limit");
            units.append(encoded.data.data(),encoded.units);at+=decoded.units;
        }
        auto name=NativeName::from_windows(std::move(units));
        return name ? Result::available(std::move(*name)) : unavailable(ObservationState::Unsupported,AdapterError::UnrepresentableName,"name.component_boundary");
    }
    const auto& units=*source.windows_units();std::string bytes;
    bytes.reserve(std::min(units.size(),budget));
    for(std::size_t at=0;at<units.size();) {
        const auto decoded=detail::decode_utf16_scalar(std::u16string_view(units).substr(at));
        if(decoded.units==0) return unavailable(ObservationState::Unsupported,AdapterError::UnrepresentableName,"name.unpaired_utf16");
        const auto encoded=detail::encode_utf8_scalar(decoded.value);
        if(encoded.units==0) return unavailable(ObservationState::Failed,AdapterError::InvalidNativeResult,"name.codec_result");
        if(encoded.units>budget-bytes.size()) return unavailable(ObservationState::Unknown,AdapterError::InventoryLimit,"name.output_limit");
        bytes.append(encoded.data.data(),encoded.units);at+=decoded.units;
    }
    auto name=NativeName::from_posix(std::move(bytes));
    return name ? Result::available(std::move(*name)) : unavailable(ObservationState::Unsupported,AdapterError::UnrepresentableName,"name.component_boundary");
}
} // namespace ck::files
