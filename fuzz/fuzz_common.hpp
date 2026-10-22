// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace ck::fuzz {

inline int hex_value(std::uint8_t byte) noexcept {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

inline std::string decode_seed_escapes(const std::uint8_t* data, std::size_t size) {
    std::string result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        if (data[index] == '\\' && index + 1 < size) {
            const std::uint8_t escaped = data[index + 1];
            if (escaped == 'e') {
                result += '\x1b';
                ++index;
                continue;
            }
            if (escaped == 'a') {
                result += '\x07';
                ++index;
                continue;
            }
            if (escaped == 'x' && index + 3 < size) {
                const int high = hex_value(data[index + 2]);
                const int low = hex_value(data[index + 3]);
                if (high >= 0 && low >= 0) {
                    result += static_cast<char>((high << 4) | low);
                    index += 3;
                    continue;
                }
            }
        }
        result += static_cast<char>(data[index]);
    }
    return result;
}

[[noreturn]] inline void invariant_failure() { std::abort(); }

inline void require(bool value) {
    if (!value) invariant_failure();
}

inline bool contains_terminal_control(std::string_view text) {
    for (const unsigned char byte : text)
        if (byte == 0x1B || byte == 0x07) return true;
    return false;
}

}  // namespace ck::fuzz
