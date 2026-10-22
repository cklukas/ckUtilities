// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_common.hpp"
#include "todo_codec.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);
    const ck::todo::TodoDecodeResult decoded = ck::todo::decode_workspace(input);
    if (!decoded) return 0;

    const ck::todo::TodoEncodeResult canonical = ck::todo::encode_workspace(*decoded.value);
    ck::fuzz::require(static_cast<bool>(canonical));
    const ck::todo::TodoDecodeResult decoded_canonical = ck::todo::decode_workspace(*canonical.value);
    ck::fuzz::require(static_cast<bool>(decoded_canonical));
    ck::fuzz::require(decoded_canonical.value->snapshot() == decoded.value->snapshot());
    const ck::todo::TodoEncodeResult second = ck::todo::encode_workspace(*decoded_canonical.value);
    ck::fuzz::require(static_cast<bool>(second));
    ck::fuzz::require(*second.value == *canonical.value);
    return 0;
}
