// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_name.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ck::files {
namespace {

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
    } else if (scalar < 0x80U) {
        text += static_cast<char>(scalar);
    } else if (scalar < 0x800U) {
        text += static_cast<char>(0xC0U | (scalar >> 6U));
        text += static_cast<char>(0x80U | (scalar & 0x3FU));
    } else if (scalar < 0x10000U) {
        text += static_cast<char>(0xE0U | (scalar >> 12U));
        text += static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU));
        text += static_cast<char>(0x80U | (scalar & 0x3FU));
    } else {
        text += static_cast<char>(0xF0U | (scalar >> 18U));
        text += static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU));
        text += static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU));
        text += static_cast<char>(0x80U | (scalar & 0x3FU));
    }
}

struct DecodedScalar {
    std::uint32_t value = 0;
    std::size_t bytes = 0;
};

// RFC 3629: shortest encoding, at most U+10FFFF, no surrogate scalars.
// A malformed sequence consumes no bytes; the caller escapes its first byte
// and retries the remainder without dropping or replacing any source byte.
DecodedScalar decode(std::string_view bytes) noexcept {
    const auto first = static_cast<unsigned char>(bytes.front());
    if (first < 0x80U) return {first, 1};
    std::size_t count = 0;
    std::uint32_t minimum = 0;
    std::uint32_t value = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        count = 2; minimum = 0x80U; value = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        count = 3; minimum = 0x800U; value = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        count = 4; minimum = 0x10000U; value = first & 0x07U;
    } else {
        return {};
    }
    if (bytes.size() < count) return {};
    for (std::size_t index = 1; index < count; ++index) {
        const auto next = static_cast<unsigned char>(bytes[index]);
        if ((next & 0xC0U) != 0x80U) return {};
        value = (value << 6U) | (next & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU))
        return {};
    return {value, count};
}

} // namespace

NativeName::NativeName(std::string bytes) : value_(std::move(bytes)) {}
NativeName::NativeName(std::u16string units) : value_(std::move(units)) {}

std::optional<NativeName> NativeName::from_posix(std::string bytes) {
    if (bytes.empty() || bytes == "." || bytes == ".." ||
        bytes.find('\0') != std::string::npos || bytes.find('/') != std::string::npos)
        return std::nullopt;
    return NativeName(std::move(bytes));
}

std::optional<NativeName> NativeName::from_windows(std::u16string units) {
    if (units.empty() || units == u"." || units == u".." ||
        units.find(u'\0') != std::u16string::npos || units.find(u'/') != std::u16string::npos ||
        units.find(u'\\') != std::u16string::npos)
        return std::nullopt;
    return NativeName(std::move(units));
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

std::string NativeName::display_name() const {
    std::string text;
    if (const auto* bytes = posix_bytes()) {
        std::size_t offset = 0;
        while (offset < bytes->size()) {
            const auto decoded = decode(std::string_view(*bytes).substr(offset));
            if (decoded.bytes == 0) {
                append_byte_escape(text, static_cast<unsigned char>((*bytes)[offset]));
                ++offset;
            } else {
                append_scalar(text, decoded.value);
                offset += decoded.bytes;
            }
        }
    } else {
        const auto& units = *windows_units();
        for (std::size_t index = 0; index < units.size(); ++index) {
            const std::uint32_t unit = units[index];
            if (unit >= 0xD800U && unit <= 0xDBFFU && index + 1 < units.size() &&
                units[index + 1] >= 0xDC00U && units[index + 1] <= 0xDFFFU) {
                const std::uint32_t scalar = 0x10000U + ((unit - 0xD800U) << 10U) +
                                             (static_cast<std::uint32_t>(units[index + 1]) - 0xDC00U);
                append_scalar(text, scalar);
                ++index;
            } else if (unit >= 0xD800U && unit <= 0xDFFFU) {
                append_scalar_escape(text, unit);
            } else {
                append_scalar(text, unit);
            }
        }
    }
    return text;
}

} // namespace ck::files
