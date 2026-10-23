// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "unicode_scalar.hpp"

namespace ck::files::detail {
namespace {
bool scalar(std::uint32_t value) noexcept { return value <= 0x10ffffU && (value < 0xd800U || value > 0xdfffU); }
}
DecodedScalar decode_utf8_scalar(std::string_view bytes) noexcept {
    if (bytes.empty()) return {};
    const auto first = static_cast<unsigned char>(bytes.front());
    if (first < 0x80U) return {first,1};
    std::size_t count = 0;
    std::uint32_t minimum = 0, value = 0;
    if (first >= 0xc2U && first <= 0xdfU) { count=2;minimum=0x80U;value=first&0x1fU; }
    else if (first >= 0xe0U && first <= 0xefU) { count=3;minimum=0x800U;value=first&0x0fU; }
    else if (first >= 0xf0U && first <= 0xf4U) { count=4;minimum=0x10000U;value=first&0x07U; }
    else return {};
    if (bytes.size() < count) return {};
    for (std::size_t i=1;i<count;++i) {
        const auto next=static_cast<unsigned char>(bytes[i]);
        if ((next&0xc0U)!=0x80U) return {};
        value=(value<<6U)|(next&0x3fU);
    }
    return value>=minimum&&scalar(value) ? DecodedScalar{value,count} : DecodedScalar{};
}
DecodedScalar decode_utf16_scalar(std::u16string_view units) noexcept {
    if (units.empty()) return {};
    const std::uint32_t first=units.front();
    if (first<0xd800U||first>0xdfffU) return {first,1};
    if (first>0xdbffU||units.size()<2||units[1]<0xdc00U||units[1]>0xdfffU) return {};
    return {0x10000U+((first-0xd800U)<<10U)+(static_cast<std::uint32_t>(units[1])-0xdc00U),2};
}
EncodedScalar<char,4> encode_utf8_scalar(std::uint32_t value) noexcept {
    if (!scalar(value)) return {};
    EncodedScalar<char,4> result;
    if (value<0x80U) { result.data[0]=static_cast<char>(value);result.units=1; }
    else if (value<0x800U) {
        result.data[0]=static_cast<char>(0xc0U|(value>>6U));result.data[1]=static_cast<char>(0x80U|(value&0x3fU));result.units=2;
    } else if (value<0x10000U) {
        result.data[0]=static_cast<char>(0xe0U|(value>>12U));result.data[1]=static_cast<char>(0x80U|((value>>6U)&0x3fU));
        result.data[2]=static_cast<char>(0x80U|(value&0x3fU));result.units=3;
    } else {
        result.data[0]=static_cast<char>(0xf0U|(value>>18U));result.data[1]=static_cast<char>(0x80U|((value>>12U)&0x3fU));
        result.data[2]=static_cast<char>(0x80U|((value>>6U)&0x3fU));result.data[3]=static_cast<char>(0x80U|(value&0x3fU));result.units=4;
    }
    return result;
}
EncodedScalar<char16_t,2> encode_utf16_scalar(std::uint32_t value) noexcept {
    if (!scalar(value)) return {};
    if (value<0x10000U) return {{{static_cast<char16_t>(value),0}},1};
    value-=0x10000U;
    return {{{static_cast<char16_t>(0xd800U+(value>>10U)),static_cast<char16_t>(0xdc00U+(value&0x3ffU))}},2};
}
} // namespace ck::files::detail
