// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_name.hpp"
#include "unicode_scalar.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ck::files {
namespace {

bool valid_component(std::string_view text) noexcept {
    return !text.empty()&&text!="."&&text!=".."&&text.find('\0')==std::string_view::npos&&text.find('/')==std::string_view::npos;
}
bool valid_component(std::u16string_view text) noexcept {
    return !text.empty()&&text!=u"."&&text!=u".."&&text.find(u'\0')==std::u16string_view::npos&&
        text.find(u'/')==std::u16string_view::npos&&text.find(u'\\')==std::u16string_view::npos;
}
constexpr char hex_digits[] = "0123456789ABCDEF";

void append_byte_escape(std::string& text, unsigned char byte) {
    text += "\\x";
    text += hex_digits[byte >> 4U];
    text += hex_digits[byte & 0x0FU];
}

void append_scalar_escape(std::string& text, std::uint32_t scalar) {
    text += "\\u{";
    const int digits = scalar > 0xFFFFU ? (scalar > 0xFFFFFU ? 6 : 5) : 4;
    for (int position = digits - 1; position >= 0; --position)
        text += hex_digits[(scalar >> (static_cast<unsigned int>(position) * 4U)) & 0x0FU];
    text += '}';
}

bool unsafe_scalar(std::uint32_t scalar) noexcept {
    return scalar < 0x20U || (scalar >= 0x7FU && scalar <= 0x9FU) ||
           scalar == 0x061CU || scalar == 0x200EU || scalar == 0x200FU ||
           (scalar >= 0x2028U && scalar <= 0x202EU) ||
           (scalar >= 0x2066U && scalar <= 0x2069U) || scalar == 0xFEFFU ||
           (scalar >= 0xFDD0U && scalar <= 0xFDEFU) ||
           (scalar & 0xFFFFU) == 0xFFFEU || (scalar & 0xFFFFU) == 0xFFFFU;
}

void append_scalar(std::string& text, std::uint32_t scalar) {
    if (scalar == static_cast<std::uint32_t>('\\')) {
        text += "\\\\";
    } else if (unsafe_scalar(scalar)) {
        append_scalar_escape(text, scalar);
    } else {
        const auto encoded=detail::encode_utf8_scalar(scalar);
        if(encoded.units==0)append_scalar_escape(text,scalar);
        else text.append(encoded.data.data(),encoded.units);
    }
}

} // namespace

NativeName::NativeName(std::string bytes) : value_(std::move(bytes)) {}
NativeName::NativeName(std::u16string units) : value_(std::move(units)) {}

std::optional<NativeName> NativeName::from_posix(std::string bytes) {
    if (!valid_component(std::string_view(bytes))) return std::nullopt;
    return NativeName(std::move(bytes));
}

std::optional<NativeName> NativeName::from_windows(std::u16string units) {
    if (!valid_component(std::u16string_view(units))) return std::nullopt;
    return NativeName(std::move(units));
}

bool NativeName::is_valid() const noexcept {
    if(const auto* bytes=posix_bytes())return valid_component(std::string_view(*bytes));
    if(const auto* units=windows_units())return valid_component(std::u16string_view(*units));
    return false;
}

NameEncoding NativeName::encoding() const noexcept {
    return std::holds_alternative<std::string>(value_) ? NameEncoding::PosixBytes : NameEncoding::WindowsUtf16;
}

const std::string* NativeName::posix_bytes() const noexcept {
    return std::get_if<std::string>(&value_);
}

const std::u16string* NativeName::windows_units() const noexcept {
    return std::get_if<std::u16string>(&value_);
}

std::string display_native_text(std::string_view bytes) {
    std::string text;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto decoded = detail::decode_utf8_scalar(bytes.substr(offset));
        if (decoded.units == 0) {
            append_byte_escape(text, static_cast<unsigned char>(bytes[offset]));
            ++offset;
        } else {
            append_scalar(text, decoded.value);
            offset += decoded.units;
        }
    }
    return text;
}

std::string display_native_text(std::u16string_view units) {
    std::string text;
    for (std::size_t index=0;index<units.size();) {
        const auto decoded=detail::decode_utf16_scalar(units.substr(index));
        if(decoded.units==0){append_scalar_escape(text,units[index]);++index;}
        else {append_scalar(text,decoded.value);index+=decoded.units;}
    }
    return text;
}

std::string NativeName::display_name() const {
    if (const auto* bytes = posix_bytes()) return display_native_text(std::string_view(*bytes));
    return display_native_text(std::u16string_view(*windows_units()));
}

} // namespace ck::files
