// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_channels.hpp"

#include <string_view>
#include <utility>

namespace ck::files {
std::optional<NativeFeatureIdentifier> NativeFeatureIdentifier::from_posix(std::string bytes) {
    if (bytes.empty() || bytes.find('\0') != std::string::npos) return std::nullopt;
    return NativeFeatureIdentifier(std::move(bytes));
}
std::optional<NativeFeatureIdentifier> NativeFeatureIdentifier::from_windows(std::u16string units) {
    if (units.empty() || units.find(u'\0') != std::u16string::npos) return std::nullopt;
    return NativeFeatureIdentifier(std::move(units));
}
NameEncoding NativeFeatureIdentifier::encoding() const noexcept {
    return posix_bytes() ? NameEncoding::PosixBytes : NameEncoding::WindowsUtf16;
}
const std::string* NativeFeatureIdentifier::posix_bytes() const noexcept { return std::get_if<std::string>(&value_); }
const std::u16string* NativeFeatureIdentifier::windows_units() const noexcept { return std::get_if<std::u16string>(&value_); }
std::string NativeFeatureIdentifier::display_name() const {
    if (const auto* bytes = posix_bytes()) return display_native_text(std::string_view(*bytes));
    return display_native_text(std::u16string_view(*windows_units()));
}
bool InventoryLimits::valid() const noexcept {
    return max_native_bytes != 0 && max_native_bytes <= hard_native_bytes &&
           max_records != 0 && max_records <= hard_records;
}
} // namespace ck::files
