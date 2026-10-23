// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_name.hpp"

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
} // namespace ck::files
