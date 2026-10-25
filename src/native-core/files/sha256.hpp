// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ck::files {

class Sha256 final {
public:
    Sha256() noexcept;

    void update(std::span<const std::byte> bytes) noexcept;
    std::array<std::byte, 32> digest() const noexcept;
    void reset() noexcept;

private:
    void transform(const std::byte *block) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t bits_ = 0;
};

} // namespace ck::files
