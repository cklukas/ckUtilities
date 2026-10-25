// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ck::files {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
    return (value >> count) | (value << (32 - count));
}
constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}
constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}
constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
    return rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
}
constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
    return rotate_right(x, 17) ^ rotate_right(x, 19) ^ (x >> 10);
}
constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
    return rotate_right(x, 2) ^ rotate_right(x, 13) ^ rotate_right(x, 22);
}
constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
    return rotate_right(x, 6) ^ rotate_right(x, 11) ^ rotate_right(x, 25);
}
std::uint32_t word(const std::byte *bytes) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[0])) << 24) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[1])) << 16) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[2])) << 8) |
        static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[3]));
}

} // namespace

Sha256::Sha256() noexcept { reset(); }

void Sha256::reset() noexcept {
    state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    buffer_.fill(std::byte{});
    buffered_ = 0;
    bits_ = 0;
}

void Sha256::transform(const std::byte *block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index != 16; ++index)
        schedule[index] = word(block + index * 4);
    for (std::size_t index = 16; index != schedule.size(); ++index)
        schedule[index] = small_sigma1(schedule[index - 2]) + schedule[index - 7] +
            small_sigma0(schedule[index - 15]) + schedule[index - 16];
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t index = 0; index != schedule.size(); ++index) {
        const auto t1 = h + big_sigma1(e) + choose(e, f, g) + round_constants[index] + schedule[index];
        const auto t2 = big_sigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() > (std::numeric_limits<std::uint64_t>::max() - bits_) / 8)
        return;
    bits_ += static_cast<std::uint64_t>(bytes.size()) * 8;
    while (!bytes.empty()) {
        const auto count = std::min<std::size_t>(bytes.size(), buffer_.size() - buffered_);
        std::copy_n(bytes.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += count;
        bytes = bytes.subspan(count);
        if (buffered_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_ = 0;
        }
    }
}

std::array<std::byte, 32> Sha256::digest() const noexcept {
    auto copy = *this;
    const std::array<std::byte, 1> one{std::byte{0x80}};
    copy.update(one);
    const std::array<std::byte, 64> zeros{};
    while (copy.buffered_ != 56)
        copy.update(std::span<const std::byte>(zeros).first(copy.buffered_ < 56 ? 56 - copy.buffered_ : 64 - copy.buffered_));
    std::array<std::byte, 8> length{};
    for (unsigned index = 0; index != 8; ++index)
        length[index] = std::byte{static_cast<unsigned char>((bits_ >> (56 - index * 8)) & 0xffU)};
    copy.update(length);
    std::array<std::byte, 32> result{};
    for (std::size_t index = 0; index != copy.state_.size(); ++index)
        for (unsigned shift = 0; shift != 4; ++shift)
            result[index * 4 + shift] = std::byte{static_cast<unsigned char>((copy.state_[index] >> (24 - shift * 8)) & 0xffU)};
    return result;
}

} // namespace ck::files
