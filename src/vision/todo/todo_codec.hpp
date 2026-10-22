// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "todo_model.hpp"

namespace ck::todo {

struct TodoCodecLimits {
    static constexpr std::size_t max_input_bytes = 16 * 1024 * 1024;
    static constexpr std::size_t max_output_bytes = 16 * 1024 * 1024;
    static constexpr std::size_t max_depth = 32;
    static constexpr std::size_t max_generic_string_bytes = 1024 * 1024;
};

enum class TodoCodecErrorCode {
    None,
    InputTooLarge,
    OutputTooLarge,
    InvalidUtf8,
    InvalidJson,
    NestingTooDeep,
    StringTooLong,
    DuplicateField,
    MissingField,
    TypeMismatch,
    IntegerOutOfRange,
    UnsupportedSchemaVersion,
    LimitExceeded,
    InvalidValue,
    InvalidWorkspace,
};

struct TodoCodecError {
    TodoCodecErrorCode code = TodoCodecErrorCode::None;
    std::size_t byte_offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
    std::string path;
    std::string diagnostic;
    friend bool operator==(const TodoCodecError&, const TodoCodecError&) = default;
};

struct TodoDecodeResult {
    std::optional<TodoWorkspace> value;
    TodoCodecError error;
    explicit operator bool() const noexcept { return value.has_value(); }
};

struct TodoEncodeResult {
    std::optional<std::string> value;
    TodoCodecError error;
    explicit operator bool() const noexcept { return value.has_value(); }
};

TodoDecodeResult decode_workspace(std::string_view json);
TodoEncodeResult encode_workspace(const TodoWorkspace& workspace);
TodoEncodeResult encode_archive_record(const ArchivedTask& record);

}  // namespace ck::todo
