// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ck::files {
enum class Win32NameIssue : std::uint32_t {
    Empty=1, Navigation=2, ForbiddenCharacter=4, DeviceAliasRisk=8, TrailingDotOrSpace=16
};
struct Win32NameCheck {
    std::uint32_t issues = 0;
    std::optional<std::size_t> first_forbidden_unit;
    bool has(Win32NameIssue issue) const noexcept { return (issues&static_cast<std::uint32_t>(issue))!=0; }
    // Only generic Win32 conventions, not filesystem acceptance, collision
    // freedom, a path-length guarantee or authority to create/access a name.
    bool ordinary_component() const noexcept { return issues==0; }
};
Win32NameCheck check_win32_component(std::u16string_view units) noexcept;
namespace detail {
// Shared with the Windows absolute-path inspection boundary. A literal counted
// NT child name can remain inspectable even if Win32 would interpret/alter it.
// Units are char16_t or the Windows native wchar_t; no locale conversion.
template<class Unit> bool win32_device_component(std::basic_string_view<Unit> text) noexcept {
    const auto upper=[](Unit unit) { const auto value=static_cast<std::uint32_t>(unit);return value>='a'&&value<='z'?value-('a'-'A'):value; };
    const auto equals=[&](std::basic_string_view<Unit> a,std::string_view b) {
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i)if(upper(a[i])!=static_cast<unsigned char>(b[i]))return false;
        return true;
    };
    if(equals(text,".")||equals(text,".."))return false;
    while(!text.empty()&&(text.back()==static_cast<Unit>(' ')||text.back()==static_cast<Unit>('.')))text.remove_suffix(1);
    std::size_t end=0;
    while(end<text.size()&&text[end]!=static_cast<Unit>('.')&&text[end]!=static_cast<Unit>(':'))++end;
    text=text.substr(0,end);
    while(!text.empty()&&text.back()==static_cast<Unit>(' '))text.remove_suffix(1);
    if(equals(text,"CON")||equals(text,"PRN")||equals(text,"AUX")||equals(text,"NUL")||equals(text,"CONIN$")||equals(text,"CONOUT$"))return true;
    if(text.size()!=4||(!equals(text.substr(0,3),"COM")&&!equals(text.substr(0,3),"LPT")))return false;
    const auto digit=static_cast<std::uint32_t>(text[3]);
    return (digit>='1'&&digit<='9')||digit==0xb9U||digit==0xb2U||digit==0xb3U;
}
}
} // namespace ck::files
