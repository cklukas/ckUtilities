// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ck::files::detail {
struct DecodedScalar { std::uint32_t value = 0; std::size_t units = 0; };
template<class Unit, std::size_t Capacity> struct EncodedScalar {
    std::array<Unit, Capacity> data{};
    std::size_t units = 0;
};
// Strict scalar codecs. Zero consumed/produced units means invalid or empty;
// no replacement, normalization, BOM stripping or partial sequence consumption.
DecodedScalar decode_utf8_scalar(std::string_view bytes) noexcept;
DecodedScalar decode_utf16_scalar(std::u16string_view units) noexcept;
EncodedScalar<char,4> encode_utf8_scalar(std::uint32_t value) noexcept;
EncodedScalar<char16_t,2> encode_utf16_scalar(std::uint32_t value) noexcept;
} // namespace ck::files::detail
