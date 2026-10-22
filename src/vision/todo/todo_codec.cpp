// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "cvision/core/utf8.hpp"

namespace ck::todo {
namespace {

class OutputBuilder {
public:
    bool append(std::string_view text) {
        if (failed_ || text.size() > TodoCodecLimits::max_output_bytes - output_.size()) {
            failed_ = true;
            return false;
        }
        output_.append(text);
        return true;
    }

    bool append(char value) {
        if (failed_ || output_.size() == TodoCodecLimits::max_output_bytes) {
            failed_ = true;
            return false;
        }
        output_.push_back(value);
        return true;
    }

    bool append_uint(std::uint64_t value) {
        std::array<char, 20> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return converted.ec == std::errc{} &&
               append(std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }

    bool append_string(std::string_view value) {
        static constexpr char hex[] = "0123456789ABCDEF";
        if (!append('"')) return false;
        for (const unsigned char byte : value) {
            switch (byte) {
                case '"':
                    if (!append("\\\"")) return false;
                    break;
                case '\\':
                    if (!append("\\\\")) return false;
                    break;
                case '\b':
                    if (!append("\\b")) return false;
                    break;
                case '\f':
                    if (!append("\\f")) return false;
                    break;
                case '\n':
                    if (!append("\\n")) return false;
                    break;
                case '\r':
                    if (!append("\\r")) return false;
                    break;
                case '\t':
                    if (!append("\\t")) return false;
                    break;
                default:
                    if (byte < 0x20U) {
                        const std::array<char, 6> escaped = {'\\', 'u', '0', '0', hex[byte >> 4U], hex[byte & 0x0FU]};
                        if (!append(std::string_view(escaped.data(), escaped.size()))) return false;
                    } else if (!append(static_cast<char>(byte))) {
                        return false;
                    }
            }
        }
        return append('"');
    }

    bool failed() const noexcept { return failed_; }
    std::string take() { return std::move(output_); }

private:
    std::string output_;
    bool failed_ = false;
};

std::string_view color_name(TodoColor color) noexcept {
    switch (color) {
        case TodoColor::Black: return "black";
        case TodoColor::DarkBlue: return "dark_blue";
        case TodoColor::DarkGreen: return "dark_green";
        case TodoColor::DarkCyan: return "dark_cyan";
        case TodoColor::DarkRed: return "dark_red";
        case TodoColor::DarkMagenta: return "dark_magenta";
        case TodoColor::Brown: return "brown";
        case TodoColor::LightGray: return "light_gray";
        case TodoColor::DarkGray: return "dark_gray";
        case TodoColor::Blue: return "blue";
        case TodoColor::Green: return "green";
        case TodoColor::Cyan: return "cyan";
        case TodoColor::Red: return "red";
        case TodoColor::Magenta: return "magenta";
        case TodoColor::Yellow: return "yellow";
        case TodoColor::White: return "white";
    }
    return {};
}

std::string_view sort_name(SortMode sort) noexcept {
    switch (sort) {
        case SortMode::Manual: return "manual";
        case SortMode::Color: return "color";
        case SortMode::Due: return "due";
        case SortMode::Created: return "created";
        case SortMode::Modified: return "modified";
        case SortMode::Priority: return "priority";
    }
    return {};
}

bool append_optional_color(OutputBuilder& out, const std::optional<TodoColor>& color) {
    return color ? out.append_string(color_name(*color)) : out.append("null");
}

bool encode_lane(OutputBuilder& out, const Lane& lane, std::size_t indent) {
    const std::string prefix(indent, ' ');
    const std::string inner(indent + 2, ' ');
    if (!out.append("{\n") || !out.append(inner) || !out.append("\"id\": ") || !out.append_uint(lane.id.value) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"title\": ") || !out.append_string(lane.title) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"color\": ") ||
        !append_optional_color(out, lane.color) || !out.append(",\n") || !out.append(inner) ||
        !out.append("\"sort\": ") || !out.append_string(sort_name(lane.sort)) || !out.append(",\n") ||
        !out.append(inner) || !out.append("\"task_ids\": [")) {
        return false;
    }
    for (std::size_t index = 0; index < lane.task_ids.size(); ++index) {
        if (index != 0 && !out.append(", ")) return false;
        if (!out.append_uint(lane.task_ids[index].value)) return false;
    }
    return out.append("]\n") && out.append(prefix) && out.append('}');
}

bool encode_board(OutputBuilder& out, const Board& board, std::size_t indent) {
    const std::string prefix(indent, ' ');
    const std::string inner(indent + 2, ' ');
    if (!out.append("{\n") || !out.append(inner) || !out.append("\"id\": ") || !out.append_uint(board.id.value) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"name\": ") || !out.append_string(board.name) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"lanes\": [")) {
        return false;
    }
    if (!board.lanes.empty() && !out.append('\n')) return false;
    for (std::size_t index = 0; index < board.lanes.size(); ++index) {
        if (!out.append(std::string(indent + 4, ' ')) || !encode_lane(out, board.lanes[index], indent + 4)) return false;
        if (index + 1 != board.lanes.size() && !out.append(',')) return false;
        if (!out.append('\n')) return false;
    }
    return out.append(inner) && out.append("]\n") && out.append(prefix) && out.append('}');
}

bool encode_task(OutputBuilder& out, const Task& task, std::size_t indent) {
    const std::string prefix(indent, ' ');
    const std::string inner(indent + 2, ' ');
    if (!out.append("{\n") || !out.append(inner) || !out.append("\"id\": ") || !out.append_uint(task.id.value) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"title\": ") || !out.append_string(task.title) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"details\": ") ||
        !out.append_string(task.details) || !out.append(",\n") || !out.append(inner) || !out.append("\"note\": ") ||
        !out.append_string(task.note) || !out.append(",\n") || !out.append(inner) ||
        !out.append("\"priority\": ") || !out.append_uint(static_cast<std::uint8_t>(task.priority)) ||
        !out.append(",\n") || !out.append(inner) || !out.append("\"due_date\": ")) {
        return false;
    }
    if (task.due_date) {
        if (!out.append_string(task.due_date->value)) return false;
    } else if (!out.append("null")) {
        return false;
    }
    if (!out.append(",\n") || !out.append(inner) || !out.append("\"due_time\": ")) return false;
    if (task.due_time) {
        if (!out.append_string(task.due_time->value)) return false;
    } else if (!out.append("null")) {
        return false;
    }
    return out.append(",\n") && out.append(inner) && out.append("\"color\": ") &&
           append_optional_color(out, task.color) && out.append(",\n") && out.append(inner) &&
           out.append("\"created_at\": ") && out.append_string(task.created_at.value) && out.append(",\n") &&
           out.append(inner) && out.append("\"created_by\": ") && out.append_string(task.created_by) &&
           out.append(",\n") && out.append(inner) && out.append("\"modified_at\": ") &&
           out.append_string(task.modified_at.value) && out.append(",\n") && out.append(inner) &&
           out.append("\"modified_by\": ") && out.append_string(task.modified_by) && out.append('\n') &&
           out.append(prefix) && out.append('}');
}

TodoCodecError output_error() {
    return TodoCodecError{TodoCodecErrorCode::OutputTooLarge,
                          TodoCodecLimits::max_output_bytes,
                          1,
                          TodoCodecLimits::max_output_bytes + 1,
                          "$",
                          "canonical TODO JSON exceeds the output byte limit"};
}

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    std::size_t position() const noexcept { return position_; }
    const TodoCodecError& error() const noexcept { return error_; }
    bool ok() const noexcept { return error_.code == TodoCodecErrorCode::None; }

    void skip_whitespace() noexcept {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\r' ||
                input_[position_] == '\n')) {
            ++position_;
        }
    }

    char peek() {
        skip_whitespace();
        return position_ < input_.size() ? input_[position_] : '\0';
    }

    bool expect(char expected, std::string_view path) {
        skip_whitespace();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return fail(TodoCodecErrorCode::InvalidJson,
                        std::string("expected '") + expected + "'",
                        path);
        }
        ++position_;
        return true;
    }

    bool fail(TodoCodecErrorCode code, std::string diagnostic, std::string_view path) {
        return fail_at(code, std::move(diagnostic), path, position_);
    }

    bool fail_at(TodoCodecErrorCode code,
                 std::string diagnostic,
                 std::string_view path,
                 std::size_t byte_offset) {
        if (!ok()) return false;
        error_.code = code;
        error_.byte_offset = std::min(byte_offset, input_.size());
        error_.path = std::string(path);
        error_.diagnostic = std::move(diagnostic);
        error_.line = 1;
        error_.column = 1;
        for (std::size_t index = 0; index < error_.byte_offset; ++index) {
            if (input_[index] == '\n') {
                ++error_.line;
                error_.column = 1;
            } else {
                ++error_.column;
            }
        }
        return false;
    }

    std::optional<std::string> string(std::size_t limit,
                                      std::string_view path,
                                      TodoCodecErrorCode expected_code = TodoCodecErrorCode::TypeMismatch) {
        skip_whitespace();
        if (position_ >= input_.size() || input_[position_] != '"') {
            fail(expected_code, "expected a JSON string", path);
            return std::nullopt;
        }
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') return result;
            if (byte < 0x20U) {
                fail(TodoCodecErrorCode::InvalidJson, "unescaped control byte in JSON string", path);
                return std::nullopt;
            }
            if (byte != '\\') {
                if (result.size() == limit) {
                    fail(TodoCodecErrorCode::StringTooLong, "decoded JSON string exceeds its byte limit", path);
                    return std::nullopt;
                }
                result.push_back(static_cast<char>(byte));
                continue;
            }
            if (position_ >= input_.size()) {
                fail(TodoCodecErrorCode::InvalidJson, "truncated JSON escape", path);
                return std::nullopt;
            }
            const char escaped = input_[position_++];
            char decoded = '\0';
            switch (escaped) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    const auto first = unicode_escape(path);
                    if (!first) return std::nullopt;
                    char32_t codepoint = *first;
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (position_ + 2U > input_.size() || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') {
                            fail(TodoCodecErrorCode::InvalidJson, "high surrogate has no low surrogate", path);
                            return std::nullopt;
                        }
                        position_ += 2;
                        const auto second = unicode_escape(path);
                        if (!second) return std::nullopt;
                        if (*second < 0xDC00 || *second > 0xDFFF) {
                            fail(TodoCodecErrorCode::InvalidJson, "invalid low surrogate", path);
                            return std::nullopt;
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10U) + (*second - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        fail(TodoCodecErrorCode::InvalidJson, "unpaired low surrogate", path);
                        return std::nullopt;
                    }
                    std::string encoded;
                    ckv::utf8::encode(codepoint, encoded);
                    if (encoded.size() > limit - std::min(limit, result.size())) {
                        fail(TodoCodecErrorCode::StringTooLong, "decoded JSON string exceeds its byte limit", path);
                        return std::nullopt;
                    }
                    result.append(encoded);
                    continue;
                }
                default:
                    fail(TodoCodecErrorCode::InvalidJson, "unknown JSON string escape", path);
                    return std::nullopt;
            }
            if (result.size() == limit) {
                fail(TodoCodecErrorCode::StringTooLong, "decoded JSON string exceeds its byte limit", path);
                return std::nullopt;
            }
            result.push_back(decoded);
        }
        fail(TodoCodecErrorCode::InvalidJson, "unterminated JSON string", path);
        return std::nullopt;
    }

    std::optional<std::uint64_t> uint64(std::string_view path) {
        skip_whitespace();
        const std::size_t start = position_;
        if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
            fail(TodoCodecErrorCode::TypeMismatch, "expected an unsigned integer", path);
            return std::nullopt;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                fail(TodoCodecErrorCode::InvalidJson, "JSON integer has a leading zero", path);
                return std::nullopt;
            }
        } else {
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        std::uint64_t result = 0;
        const auto converted = std::from_chars(input_.data() + start, input_.data() + position_, result);
        if (converted.ec == std::errc::result_out_of_range) {
            fail(TodoCodecErrorCode::IntegerOutOfRange, "unsigned integer is out of range", path);
            return std::nullopt;
        }
        return result;
    }

    bool null(std::string_view path) { return literal("null", TodoCodecErrorCode::TypeMismatch, path); }

    template <class Function>
    bool object(std::size_t depth, std::string_view path, Function&& member) {
        if (depth > TodoCodecLimits::max_depth) {
            return fail(TodoCodecErrorCode::NestingTooDeep, "JSON nesting limit exceeded", path);
        }
        if (!expect('{', path)) return false;
        skip_whitespace();
        if (peek() == '}') {
            ++position_;
            return true;
        }
        while (ok()) {
            auto key = string(TodoCodecLimits::max_generic_string_bytes, path, TodoCodecErrorCode::InvalidJson);
            if (!key || !expect(':', path) || !member(*key)) return false;
            skip_whitespace();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                return true;
            }
            if (!expect(',', path)) return false;
        }
        return false;
    }

    bool skip_value(std::size_t depth, std::string_view path) {
        skip_whitespace();
        if (position_ >= input_.size()) return fail(TodoCodecErrorCode::InvalidJson, "expected a JSON value", path);
        const char value = input_[position_];
        if (value == '"') return string(TodoCodecLimits::max_generic_string_bytes, path).has_value();
        if (value == '{') {
            return object(depth, path, [&](const std::string&) { return skip_value(depth + 1, path); });
        }
        if (value == '[') {
            if (depth > TodoCodecLimits::max_depth) {
                return fail(TodoCodecErrorCode::NestingTooDeep, "JSON nesting limit exceeded", path);
            }
            ++position_;
            skip_whitespace();
            if (peek() == ']') {
                ++position_;
                return true;
            }
            while (ok()) {
                if (!skip_value(depth + 1, path)) return false;
                skip_whitespace();
                if (position_ < input_.size() && input_[position_] == ']') {
                    ++position_;
                    return true;
                }
                if (!expect(',', path)) return false;
            }
            return false;
        }
        if (value == 't') return literal("true", TodoCodecErrorCode::InvalidJson, path);
        if (value == 'f') return literal("false", TodoCodecErrorCode::InvalidJson, path);
        if (value == 'n') return literal("null", TodoCodecErrorCode::InvalidJson, path);
        return skip_number(path);
    }

    bool finish(std::string_view path) {
        skip_whitespace();
        if (position_ != input_.size()) return fail(TodoCodecErrorCode::InvalidJson, "trailing JSON bytes", path);
        return true;
    }

private:
    std::optional<char32_t> unicode_escape(std::string_view path) {
        if (position_ + 4U > input_.size()) {
            fail(TodoCodecErrorCode::InvalidJson, "truncated Unicode escape", path);
            return std::nullopt;
        }
        char32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = input_[position_++];
            result <<= 4U;
            if (digit >= '0' && digit <= '9') result += digit - '0';
            else if (digit >= 'a' && digit <= 'f') result += digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F') result += digit - 'A' + 10;
            else {
                fail(TodoCodecErrorCode::InvalidJson, "invalid Unicode escape", path);
                return std::nullopt;
            }
        }
        return result;
    }

    bool literal(std::string_view literal_value, TodoCodecErrorCode code, std::string_view path) {
        skip_whitespace();
        if (literal_value.size() > input_.size() - std::min(input_.size(), position_) ||
            input_.substr(position_, literal_value.size()) != literal_value) {
            return fail(code, "unexpected JSON value", path);
        }
        position_ += literal_value.size();
        return true;
    }

    bool skip_number(std::string_view path) {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return fail(TodoCodecErrorCode::InvalidJson, "truncated JSON number", path);
        if (input_[position_] == '0') {
            ++position_;
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        } else {
            return fail(TodoCodecErrorCode::InvalidJson, "invalid JSON number", path);
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (digits == position_) return fail(TodoCodecErrorCode::InvalidJson, "invalid JSON fraction", path);
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (digits == position_) return fail(TodoCodecErrorCode::InvalidJson, "invalid JSON exponent", path);
        }
        return position_ != start;
    }

    std::string_view input_;
    std::size_t position_ = 0;
    TodoCodecError error_;
};

bool claim_field(JsonReader& reader,
                 std::uint32_t& fields,
                 std::uint32_t bit,
                 std::string_view key,
                 std::string_view path) {
    if ((fields & bit) != 0U) {
        return reader.fail(TodoCodecErrorCode::DuplicateField,
                           "duplicate field '" + std::string(key) + "'",
                           path);
    }
    fields |= bit;
    return true;
}

bool require_fields(JsonReader& reader,
                    std::uint32_t fields,
                    std::uint32_t required,
                    std::string_view path,
                    std::string_view object_name) {
    if (fields == required) return true;
    return reader.fail(TodoCodecErrorCode::MissingField,
                       std::string(object_name) + " is missing a required field",
                       path);
}

std::optional<TodoColor> parse_color_name(std::string_view value) {
    constexpr std::array<std::pair<std::string_view, TodoColor>, 16> colors = {{
        {"black", TodoColor::Black},          {"dark_blue", TodoColor::DarkBlue},
        {"dark_green", TodoColor::DarkGreen}, {"dark_cyan", TodoColor::DarkCyan},
        {"dark_red", TodoColor::DarkRed},     {"dark_magenta", TodoColor::DarkMagenta},
        {"brown", TodoColor::Brown},          {"light_gray", TodoColor::LightGray},
        {"dark_gray", TodoColor::DarkGray},   {"blue", TodoColor::Blue},
        {"green", TodoColor::Green},          {"cyan", TodoColor::Cyan},
        {"red", TodoColor::Red},              {"magenta", TodoColor::Magenta},
        {"yellow", TodoColor::Yellow},        {"white", TodoColor::White},
    }};
    const auto found = std::find_if(colors.begin(), colors.end(),
                                    [value](const auto& item) { return item.first == value; });
    return found == colors.end() ? std::nullopt : std::optional<TodoColor>{found->second};
}

std::optional<SortMode> parse_sort_name(std::string_view value) {
    if (value == "manual") return SortMode::Manual;
    if (value == "color") return SortMode::Color;
    if (value == "due") return SortMode::Due;
    if (value == "created") return SortMode::Created;
    if (value == "modified") return SortMode::Modified;
    if (value == "priority") return SortMode::Priority;
    return std::nullopt;
}

class WorkspaceParser {
public:
    explicit WorkspaceParser(JsonReader& reader) : reader_(reader) {}

    std::optional<WorkspaceSnapshot> parse() {
        WorkspaceSnapshot snapshot;
        std::uint32_t fields = 0;
        constexpr std::uint32_t required = 0x7FU;
        const bool parsed = reader_.object(1, "$", [&](const std::string& key) {
            if (key == "schema_version") {
                if (!claim_field(reader_, fields, 1U << 0U, key, "$.schema_version")) return false;
                const auto value = reader_.uint64("$.schema_version");
                if (!value) return false;
                if (*value > std::numeric_limits<std::uint32_t>::max()) {
                    return reader_.fail(TodoCodecErrorCode::IntegerOutOfRange,
                                        "schema version is out of range",
                                        "$.schema_version");
                }
                snapshot.schema_version = static_cast<std::uint32_t>(*value);
                return true;
            }
            if (key == "last_board_id") {
                if (!claim_field(reader_, fields, 1U << 1U, key, "$.last_board_id")) return false;
                const auto value = reader_.uint64("$.last_board_id");
                if (!value) return false;
                snapshot.last_board_id = BoardId{*value};
                return true;
            }
            if (key == "next_board_id") {
                return parse_counter(fields, 1U << 2U, key, "$.next_board_id", snapshot.next_board_id);
            }
            if (key == "next_lane_id") {
                return parse_counter(fields, 1U << 3U, key, "$.next_lane_id", snapshot.next_lane_id);
            }
            if (key == "next_task_id") {
                return parse_counter(fields, 1U << 4U, key, "$.next_task_id", snapshot.next_task_id);
            }
            if (key == "boards") {
                if (!claim_field(reader_, fields, 1U << 5U, key, "$.boards")) return false;
                return parse_boards(snapshot.boards);
            }
            if (key == "tasks") {
                if (!claim_field(reader_, fields, 1U << 6U, key, "$.tasks")) return false;
                return parse_tasks(snapshot.tasks);
            }
            return reader_.skip_value(2, "$.*");
        });
        if (!parsed || !require_fields(reader_, fields, required, "$", "workspace")) return std::nullopt;
        return snapshot;
    }

private:
    bool parse_counter(std::uint32_t& fields,
                       std::uint32_t bit,
                       std::string_view key,
                       std::string_view path,
                       std::uint64_t& target) {
        if (!claim_field(reader_, fields, bit, key, path)) return false;
        const auto value = reader_.uint64(path);
        if (!value) return false;
        target = *value;
        return true;
    }

    bool parse_boards(std::vector<Board>& boards) {
        if (!reader_.expect('[', "$.boards")) return false;
        if (reader_.peek() == ']') return reader_.expect(']', "$.boards");
        while (reader_.ok()) {
            if (boards.size() == TodoLimits::max_boards) {
                return reader_.fail(TodoCodecErrorCode::LimitExceeded, "board count limit exceeded", "$.boards");
            }
            auto board = parse_board();
            if (!board) return false;
            boards.push_back(std::move(*board));
            if (reader_.peek() == ']') return reader_.expect(']', "$.boards");
            if (!reader_.expect(',', "$.boards")) return false;
        }
        return false;
    }

    std::optional<Board> parse_board() {
        Board board;
        std::uint32_t fields = 0;
        constexpr std::uint32_t required = 0x7U;
        const bool parsed = reader_.object(3, "$.boards[]", [&](const std::string& key) {
            if (key == "id") {
                if (!claim_field(reader_, fields, 1U << 0U, key, "$.boards[].id")) return false;
                const auto value = reader_.uint64("$.boards[].id");
                if (!value) return false;
                board.id = BoardId{*value};
                return true;
            }
            if (key == "name") {
                if (!claim_field(reader_, fields, 1U << 1U, key, "$.boards[].name")) return false;
                auto value = reader_.string(TodoLimits::max_name_bytes, "$.boards[].name");
                if (!value) return false;
                board.name = std::move(*value);
                return true;
            }
            if (key == "lanes") {
                if (!claim_field(reader_, fields, 1U << 2U, key, "$.boards[].lanes")) return false;
                return parse_lanes(board.lanes);
            }
            return reader_.skip_value(4, "$.boards[].*");
        });
        if (!parsed || !require_fields(reader_, fields, required, "$.boards[]", "board")) return std::nullopt;
        return board;
    }

    bool parse_lanes(std::vector<Lane>& lanes) {
        if (!reader_.expect('[', "$.boards[].lanes")) return false;
        if (reader_.peek() == ']') return reader_.expect(']', "$.boards[].lanes");
        while (reader_.ok()) {
            if (lanes.size() == TodoLimits::max_lanes_per_board || total_lanes_ == TodoLimits::max_lanes) {
                return reader_.fail(TodoCodecErrorCode::LimitExceeded, "lane count limit exceeded", "$.boards[].lanes");
            }
            auto lane = parse_lane();
            if (!lane) return false;
            lanes.push_back(std::move(*lane));
            ++total_lanes_;
            if (reader_.peek() == ']') return reader_.expect(']', "$.boards[].lanes");
            if (!reader_.expect(',', "$.boards[].lanes")) return false;
        }
        return false;
    }

    std::optional<Lane> parse_lane() {
        Lane lane;
        std::uint32_t fields = 0;
        constexpr std::uint32_t required = 0x1FU;
        const bool parsed = reader_.object(5, "$.boards[].lanes[]", [&](const std::string& key) {
            if (key == "id") {
                if (!claim_field(reader_, fields, 1U << 0U, key, "$.boards[].lanes[].id")) return false;
                const auto value = reader_.uint64("$.boards[].lanes[].id");
                if (!value) return false;
                lane.id = LaneId{*value};
                return true;
            }
            if (key == "title") {
                if (!claim_field(reader_, fields, 1U << 1U, key, "$.boards[].lanes[].title")) return false;
                auto value = reader_.string(TodoLimits::max_name_bytes, "$.boards[].lanes[].title");
                if (!value) return false;
                lane.title = std::move(*value);
                return true;
            }
            if (key == "color") {
                if (!claim_field(reader_, fields, 1U << 2U, key, "$.boards[].lanes[].color")) return false;
                return parse_color(lane.color, "$.boards[].lanes[].color");
            }
            if (key == "sort") {
                if (!claim_field(reader_, fields, 1U << 3U, key, "$.boards[].lanes[].sort")) return false;
                auto value = reader_.string(16, "$.boards[].lanes[].sort");
                if (!value) return false;
                const auto parsed_sort = parse_sort_name(*value);
                if (!parsed_sort) {
                    return reader_.fail(TodoCodecErrorCode::InvalidValue,
                                        "unknown lane sort mode",
                                        "$.boards[].lanes[].sort");
                }
                lane.sort = *parsed_sort;
                return true;
            }
            if (key == "task_ids") {
                if (!claim_field(reader_, fields, 1U << 4U, key, "$.boards[].lanes[].task_ids")) return false;
                return parse_task_ids(lane.task_ids);
            }
            return reader_.skip_value(6, "$.boards[].lanes[].*");
        });
        if (!parsed || !require_fields(reader_, fields, required, "$.boards[].lanes[]", "lane")) {
            return std::nullopt;
        }
        return lane;
    }

    bool parse_task_ids(std::vector<TaskId>& task_ids) {
        if (!reader_.expect('[', "$.boards[].lanes[].task_ids")) return false;
        if (reader_.peek() == ']') return reader_.expect(']', "$.boards[].lanes[].task_ids");
        while (reader_.ok()) {
            if (total_task_references_ == TodoLimits::max_tasks) {
                return reader_.fail(TodoCodecErrorCode::LimitExceeded,
                                    "task reference count limit exceeded",
                                    "$.boards[].lanes[].task_ids");
            }
            const auto value = reader_.uint64("$.boards[].lanes[].task_ids[]");
            if (!value) return false;
            task_ids.push_back(TaskId{*value});
            ++total_task_references_;
            if (reader_.peek() == ']') return reader_.expect(']', "$.boards[].lanes[].task_ids");
            if (!reader_.expect(',', "$.boards[].lanes[].task_ids")) return false;
        }
        return false;
    }

    bool parse_tasks(std::vector<Task>& tasks) {
        if (!reader_.expect('[', "$.tasks")) return false;
        if (reader_.peek() == ']') return reader_.expect(']', "$.tasks");
        while (reader_.ok()) {
            if (tasks.size() == TodoLimits::max_tasks) {
                return reader_.fail(TodoCodecErrorCode::LimitExceeded, "task count limit exceeded", "$.tasks");
            }
            auto task = parse_task();
            if (!task) return false;
            tasks.push_back(std::move(*task));
            if (reader_.peek() == ']') return reader_.expect(']', "$.tasks");
            if (!reader_.expect(',', "$.tasks")) return false;
        }
        return false;
    }

    std::optional<Task> parse_task() {
        Task task;
        std::uint32_t fields = 0;
        constexpr std::uint32_t required = 0xFFFU;
        const bool parsed = reader_.object(3, "$.tasks[]", [&](const std::string& key) {
            if (key == "id") return parse_task_id(fields, key, task.id);
            if (key == "title") return parse_task_string(fields, 1U << 1U, key, task.title,
                                                           TodoLimits::max_title_bytes);
            if (key == "details") return parse_task_string(fields, 1U << 2U, key, task.details,
                                                             TodoLimits::max_details_bytes);
            if (key == "note") return parse_task_string(fields, 1U << 3U, key, task.note,
                                                          TodoLimits::max_note_bytes);
            if (key == "priority") return parse_priority(fields, key, task.priority);
            if (key == "due_date") return parse_due(fields, key, task.due_date);
            if (key == "due_time") return parse_due_time(fields, key, task.due_time);
            if (key == "color") {
                if (!claim_field(reader_, fields, 1U << 7U, key, "$.tasks[].color")) return false;
                return parse_color(task.color, "$.tasks[].color");
            }
            if (key == "created_at") return parse_timestamp(fields, 1U << 8U, key, task.created_at);
            if (key == "created_by") return parse_task_string(fields, 1U << 9U, key, task.created_by,
                                                                TodoLimits::max_identity_bytes);
            if (key == "modified_at") return parse_timestamp(fields, 1U << 10U, key, task.modified_at);
            if (key == "modified_by") return parse_task_string(fields, 1U << 11U, key, task.modified_by,
                                                                 TodoLimits::max_identity_bytes);
            return reader_.skip_value(4, "$.tasks[].*");
        });
        if (!parsed || !require_fields(reader_, fields, required, "$.tasks[]", "task")) return std::nullopt;
        return task;
    }

    bool parse_task_id(std::uint32_t& fields, std::string_view key, TaskId& id) {
        if (!claim_field(reader_, fields, 1U << 0U, key, "$.tasks[].id")) return false;
        const auto value = reader_.uint64("$.tasks[].id");
        if (!value) return false;
        id = TaskId{*value};
        return true;
    }

    bool parse_task_string(std::uint32_t& fields,
                           std::uint32_t bit,
                           std::string_view key,
                           std::string& target,
                           std::size_t limit) {
        const std::string path = "$.tasks[]." + std::string(key);
        if (!claim_field(reader_, fields, bit, key, path)) return false;
        auto value = reader_.string(limit, path);
        if (!value) return false;
        target = std::move(*value);
        return true;
    }

    bool parse_priority(std::uint32_t& fields, std::string_view key, Priority& priority) {
        if (!claim_field(reader_, fields, 1U << 4U, key, "$.tasks[].priority")) return false;
        const auto value = reader_.uint64("$.tasks[].priority");
        if (!value) return false;
        if (*value < static_cast<std::uint8_t>(Priority::High) ||
            *value > static_cast<std::uint8_t>(Priority::Idle)) {
            return reader_.fail(TodoCodecErrorCode::InvalidValue,
                                "task priority must be 1 through 4",
                                "$.tasks[].priority");
        }
        priority = static_cast<Priority>(*value);
        return true;
    }

    bool parse_due(std::uint32_t& fields, std::string_view key, std::optional<IsoDate>& due) {
        if (!claim_field(reader_, fields, 1U << 5U, key, "$.tasks[].due_date")) return false;
        if (reader_.peek() == 'n') return reader_.null("$.tasks[].due_date");
        auto value = reader_.string(10, "$.tasks[].due_date");
        if (!value) return false;
        due = IsoDate{std::move(*value)};
        return true;
    }

    bool parse_due_time(std::uint32_t& fields, std::string_view key, std::optional<IsoTime>& due) {
        if (!claim_field(reader_, fields, 1U << 6U, key, "$.tasks[].due_time")) return false;
        if (reader_.peek() == 'n') return reader_.null("$.tasks[].due_time");
        auto value = reader_.string(5, "$.tasks[].due_time");
        if (!value) return false;
        due = IsoTime{std::move(*value)};
        return true;
    }

    bool parse_timestamp(std::uint32_t& fields,
                         std::uint32_t bit,
                         std::string_view key,
                         IsoTimestamp& timestamp) {
        const std::string path = "$.tasks[]." + std::string(key);
        if (!claim_field(reader_, fields, bit, key, path)) return false;
        auto value = reader_.string(20, path);
        if (!value) return false;
        timestamp = IsoTimestamp{std::move(*value)};
        return true;
    }

    bool parse_color(std::optional<TodoColor>& color, std::string_view path) {
        if (reader_.peek() == 'n') return reader_.null(path);
        auto value = reader_.string(32, path);
        if (!value) return false;
        const auto parsed = parse_color_name(*value);
        if (!parsed) return reader_.fail(TodoCodecErrorCode::InvalidValue, "unknown TODO color", path);
        color = *parsed;
        return true;
    }

    JsonReader& reader_;
    std::size_t total_lanes_ = 0;
    std::size_t total_task_references_ = 0;
};

TodoDecodeResult reader_failure(const JsonReader& reader) { return {std::nullopt, reader.error()}; }

std::optional<std::size_t> invalid_utf8_offset(std::string_view input) noexcept {
    std::size_t position = 0;
    while (position < input.size()) {
        const std::size_t start = position;
        const char32_t codepoint = ckv::utf8::decode(input, position);
        if (codepoint == ckv::utf8::replacement_char && !ckv::utf8::is_valid(input.substr(start, position - start))) return start;
    }
    return std::nullopt;
}

TodoCodecError utf8_error(std::string_view input, std::size_t offset) {
    TodoCodecError error{TodoCodecErrorCode::InvalidUtf8, offset, 1, 1, "$", "TODO JSON is not valid UTF-8"};
    for (std::size_t index = 0; index < offset; ++index) {
        if (input[index] == '\n') {
            ++error.line;
            error.column = 1;
        } else {
            ++error.column;
        }
    }
    return error;
}

TodoDecodeResult validate_schema(std::string_view input) {
    JsonReader reader(input);
    std::optional<std::uint64_t> schema;
    std::size_t schema_offset = 0;
    bool seen = false;
    const bool parsed = reader.object(1, "$", [&](const std::string& key) {
        if (key != "schema_version") return reader.skip_value(2, "$.*");
        if (seen) return reader.fail(TodoCodecErrorCode::DuplicateField, "duplicate field 'schema_version'", "$.schema_version");
        seen = true;
        schema_offset = reader.position();
        schema = reader.uint64("$.schema_version");
        return schema.has_value();
    });
    if (!parsed || !reader.finish("$")) return reader_failure(reader);
    if (!schema) {
        reader.fail(TodoCodecErrorCode::MissingField, "workspace is missing schema_version", "$.schema_version");
        return reader_failure(reader);
    }
    if (*schema != todo_schema_version) {
        reader.fail_at(TodoCodecErrorCode::UnsupportedSchemaVersion,
                       "unsupported TODO schema version",
                       "$.schema_version",
                       schema_offset);
        return reader_failure(reader);
    }
    return {};
}

}  // namespace

TodoEncodeResult encode_workspace(const TodoWorkspace& workspace) {
    OutputBuilder out;
    const WorkspaceSnapshot& snapshot = workspace.snapshot();
    if (!out.append("{\n  \"schema_version\": ") || !out.append_uint(snapshot.schema_version) ||
        !out.append(",\n  \"last_board_id\": ") || !out.append_uint(snapshot.last_board_id.value) ||
        !out.append(",\n  \"next_board_id\": ") || !out.append_uint(snapshot.next_board_id) ||
        !out.append(",\n  \"next_lane_id\": ") || !out.append_uint(snapshot.next_lane_id) ||
        !out.append(",\n  \"next_task_id\": ") || !out.append_uint(snapshot.next_task_id) ||
        !out.append(",\n  \"boards\": [\n")) {
        return {std::nullopt, output_error()};
    }
    for (std::size_t index = 0; index < snapshot.boards.size(); ++index) {
        if (!out.append("    ") || !encode_board(out, snapshot.boards[index], 4) ||
            (index + 1 != snapshot.boards.size() && !out.append(',')) || !out.append('\n')) {
            return {std::nullopt, output_error()};
        }
    }
    if (!out.append("  ],\n  \"tasks\": [")) return {std::nullopt, output_error()};
    if (snapshot.tasks.empty()) {
        if (!out.append("]\n}\n")) return {std::nullopt, output_error()};
        return {out.take(), {}};
    }
    if (!out.append('\n')) return {std::nullopt, output_error()};
    for (std::size_t index = 0; index < snapshot.tasks.size(); ++index) {
        if (!out.append("    ") || !encode_task(out, snapshot.tasks[index], 4) ||
            (index + 1 != snapshot.tasks.size() && !out.append(',')) || !out.append('\n')) {
            return {std::nullopt, output_error()};
        }
    }
    if (!out.append("  ]\n}\n") || out.failed()) return {std::nullopt, output_error()};
    return {out.take(), {}};
}

TodoEncodeResult encode_archive_record(const ArchivedTask& record) {
    OutputBuilder out;
    if (!out.append("{\n  \"schema_version\": 1,\n  \"archived_at\": ") ||
        !out.append_string(record.archived.timestamp.value) || !out.append(",\n  \"archived_by\": ") ||
        !out.append_string(record.archived.identity) || !out.append(",\n  \"origin_board_id\": ") ||
        !out.append_uint(record.origin_board_id.value) || !out.append(",\n  \"origin_board_name\": ") ||
        !out.append_string(record.origin_board_name) || !out.append(",\n  \"origin_lane_id\": ") ||
        !out.append_uint(record.origin_lane_id.value) || !out.append(",\n  \"origin_lane_title\": ") ||
        !out.append_string(record.origin_lane_title) || !out.append(",\n  \"task\": ") ||
        !encode_task(out, record.task, 2) || !out.append("\n}\n")) {
        return {std::nullopt, output_error()};
    }
    return {out.take(), {}};
}

TodoDecodeResult decode_workspace(std::string_view json) {
    if (json.size() > TodoCodecLimits::max_input_bytes) {
        return {std::nullopt,
                TodoCodecError{TodoCodecErrorCode::InputTooLarge,
                               TodoCodecLimits::max_input_bytes,
                               1,
                               TodoCodecLimits::max_input_bytes + 1,
                               "$",
                               "TODO JSON exceeds the input byte limit"}};
    }
    if (const auto offset = invalid_utf8_offset(json)) return {std::nullopt, utf8_error(json, *offset)};
    TodoDecodeResult schema_result = validate_schema(json);
    if (schema_result.error.code != TodoCodecErrorCode::None) return schema_result;

    JsonReader reader(json);
    WorkspaceParser parser(reader);
    auto snapshot = parser.parse();
    if (!snapshot || !reader.finish("$")) return reader_failure(reader);
    auto workspace = TodoWorkspace::from_snapshot(std::move(*snapshot));
    if (!workspace) {
        reader.fail(TodoCodecErrorCode::InvalidWorkspace, std::move(workspace.error.diagnostic), "$");
        return reader_failure(reader);
    }
    return {std::move(*workspace.value), {}};
}

}  // namespace ck::todo
