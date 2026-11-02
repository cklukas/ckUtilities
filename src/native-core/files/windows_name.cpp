// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_name.hpp"

#include <algorithm>

namespace ck::files {
Win32NameCheck check_win32_component(std::u16string_view units) noexcept {
    Win32NameCheck result;
    const auto flag=[&](Win32NameIssue issue){result.issues|=static_cast<std::uint32_t>(issue);};
    if(units.empty())flag(Win32NameIssue::Empty);
    if(units==u"."||units==u"..")flag(Win32NameIssue::Navigation);
    for(std::size_t i=0;i<units.size();++i) {
        const auto unit=units[i];
        if(unit<=0x1fU||std::u16string_view(u"<>:\"/\\|?*").find(unit)!=std::u16string_view::npos) {
            flag(Win32NameIssue::ForbiddenCharacter);
            if(!result.first_forbidden_unit)result.first_forbidden_unit=i;
        }
    }
    if(detail::win32_device_component(units))flag(Win32NameIssue::DeviceAliasRisk);
    if(!units.empty()&&(units.back()==u'.'||units.back()==u' '))flag(Win32NameIssue::TrailingDotOrSpace);
    return result;
}

std::optional<std::wstring> windows_extended_path(std::wstring_view path) {
    if (path.empty() || path.find(L'\0') != path.npos || path.size() > 32767)
        return std::nullopt;
    std::wstring normalized(path);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.starts_with(L"\\\\?\\")) {
        constexpr std::wstring_view volume_prefix = L"\\\\?\\Volume{";
        constexpr std::size_t volume_guid_units = 36;
        const bool volume = normalized.starts_with(volume_prefix) &&
            normalized.size() >= volume_prefix.size() + volume_guid_units + 2 &&
            normalized[volume_prefix.size() + volume_guid_units] == L'}' &&
            normalized[volume_prefix.size() + volume_guid_units + 1] == L'\\';
        if (normalized.starts_with(L"\\\\?\\UNC\\") || volume ||
            (normalized.size() >= 7 && ((normalized[4] >= L'A' && normalized[4] <= L'Z') ||
                (normalized[4] >= L'a' && normalized[4] <= L'z')) && normalized[5] == L':' && normalized[6] == L'\\'))
            return normalized;
        return std::nullopt;
    }
    if (normalized.starts_with(L"\\\\.\\") || normalized.starts_with(L"\\??\\"))
        return std::nullopt;
    const bool drive = normalized.size() >= 3 &&
        ((normalized[0] >= L'A' && normalized[0] <= L'Z') || (normalized[0] >= L'a' && normalized[0] <= L'z')) &&
        normalized[1] == L':' && normalized[2] == L'\\';
    if (drive) return L"\\\\?\\" + normalized;
    if (normalized.size() < 5 || normalized[0] != L'\\' || normalized[1] != L'\\' || normalized[2] == L'\\')
        return std::nullopt;
    const auto server_end = normalized.find(L'\\', 2);
    if (server_end == std::wstring::npos || server_end == 2) return std::nullopt;
    const auto share_end = normalized.find(L'\\', server_end + 1);
    if (share_end == server_end + 1) return std::nullopt;
    return L"\\\\?\\UNC" + normalized.substr(1);
}
} // namespace ck::files
