// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_codec.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cvision/testing/cktest.hpp"

namespace {

using namespace ck::todo;

constexpr std::string_view empty_json = R"json({
  "schema_version": 2,
  "last_board_id": 1,
  "next_board_id": 2,
  "next_lane_id": 4,
  "next_task_id": 1,
  "boards": [
    {
      "id": 1,
      "name": "main",
      "lanes": [
        {
          "id": 1,
          "title": "To Do",
          "color": null,
          "sort": "manual",
          "task_ids": []
        },
        {
          "id": 2,
          "title": "Doing",
          "color": null,
          "sort": "manual",
          "task_ids": []
        },
        {
          "id": 3,
          "title": "Done",
          "color": null,
          "sort": "manual",
          "task_ids": []
        }
      ]
    }
  ],
  "tasks": []
}
)json";

AuditStamp codec_stamp() { return {IsoTimestamp{"2026-08-25T12:00:00Z"}, "codec-test"}; }

std::string replace_once(std::string value, std::string_view needle, std::string_view replacement) {
    const std::size_t position = value.find(needle);
    if (position != std::string::npos) value.replace(position, needle.size(), replacement);
    return value;
}

std::string encoded_empty() { return *encode_workspace(TodoWorkspace::empty()).value; }

TodoCodecErrorCode decode_error(std::string_view json) { return decode_workspace(json).error.code; }

}  // namespace

CK_TEST(todo_codec_empty_workspace_has_exact_canonical_bytes) {
    const auto encoded = encode_workspace(TodoWorkspace::empty());
    CK_CHECK(encoded);
    if (!encoded) return;
    CK_CHECK(*encoded.value == empty_json);
}

CK_TEST(todo_codec_encoding_is_deterministic_and_preserves_semantic_array_order) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    CK_CHECK(workspace.insert_lane(BoardId{1}, "First", LaneId{1}));
    const auto first = encode_workspace(workspace);
    const auto second = encode_workspace(workspace);
    CK_CHECK(first && second);
    if (!first || !second) return;
    CK_CHECK(*first.value == *second.value);
    CK_CHECK(first.value->find("\"title\": \"First\"") < first.value->find("\"title\": \"To Do\""));
}

CK_TEST(todo_codec_escapes_json_controls_but_keeps_slashes_and_unicode_readable) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    TaskDraft task;
    task.title = "Unicode \xF0\x9F\x98\x80";
    task.details = "path /tmp/todo";
    task.note = "quote \" slash \\ line\n tab\t";
    CK_CHECK(workspace.add_task(LaneId{1}, std::move(task), codec_stamp()));
    const auto encoded = encode_workspace(workspace);
    CK_CHECK(encoded);
    if (!encoded) return;
    CK_CHECK(encoded.value->find("Unicode \xF0\x9F\x98\x80") != std::string::npos);
    CK_CHECK(encoded.value->find("path /tmp/todo") != std::string::npos);
    CK_CHECK(encoded.value->find("quote \\\" slash \\\\ line\\n tab\\t") != std::string::npos);
    CK_CHECK(encoded.value->back() == '\n');
}

CK_TEST(todo_codec_empty_and_guided_workspaces_round_trip_canonically) {
    const TodoWorkspace empty = TodoWorkspace::empty();
    const auto decoded_empty = decode_workspace(*encode_workspace(empty).value);
    CK_CHECK(decoded_empty && decoded_empty.value->snapshot() == empty.snapshot());

    const auto guided = TodoWorkspace::guided(codec_stamp());
    CK_CHECK(guided);
    if (!guided) return;
    const auto first = encode_workspace(*guided.value);
    const auto decoded = decode_workspace(*first.value);
    CK_CHECK(decoded && decoded.value->snapshot() == guided.value->snapshot());
    if (!decoded) return;
    const auto second = encode_workspace(*decoded.value);
    CK_CHECK(second && *second.value == *first.value);
}

CK_TEST(todo_codec_accepts_reordered_fields_whitespace_and_nested_unknowns) {
    const std::string json = R"json({
      "unknown": {"nested": [true, false, null, -1.25e+3, {"x": "y"}]},
      "tasks": [],
      "next_task_id": 1,
      "boards": [{"lanes": [
        {"task_ids": [], "sort": "manual", "color": null, "title": "To Do", "id": 1},
        {"id": 2, "title": "Doing", "color": null, "sort": "manual", "task_ids": []},
        {"id": 3, "title": "Done", "color": null, "sort": "manual", "task_ids": []}
      ], "name": "main", "id": 1, "ignored": 7}],
      "next_lane_id": 4,
      "schema_version": 2,
      "next_board_id": 2,
      "last_board_id": 1
    })json";
    const auto decoded = decode_workspace(json);
    CK_CHECK(decoded && decoded.value->snapshot() == TodoWorkspace::empty().snapshot());
    if (!decoded) return;
    CK_CHECK(*encode_workspace(*decoded.value).value == empty_json);
}

CK_TEST(todo_codec_decodes_raw_unicode_escapes_and_surrogate_pairs) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    TaskDraft task;
    task.title = "Raw \xF0\x9F\x98\x80";
    task.details = "Gr\xC3\xBC\xC3\x9F";
    CK_CHECK(workspace.add_task(LaneId{1}, std::move(task), codec_stamp()));
    std::string json = *encode_workspace(workspace).value;
    json = replace_once(std::move(json), "Raw \xF0\x9F\x98\x80", "Escaped \\uD83D\\uDE00");
    const auto decoded = decode_workspace(json);
    CK_CHECK(decoded);
    if (!decoded) return;
    CK_CHECK(decoded.value->find_task(TaskId{1})->title == "Escaped \xF0\x9F\x98\x80");
    CK_CHECK(decoded.value->find_task(TaskId{1})->details == "Gr\xC3\xBC\xC3\x9F");
}

CK_TEST(todo_codec_rejects_malformed_json_utf8_and_surrogates_without_a_value) {
    const std::vector<std::string> malformed = {
        "",
        "[]",
        "{\"schema_version\":1,}",
        "{\"schema_version\":1 // comment\n}",
        "{\"schema_version\":1,\"x\":NaN}",
        "{\"schema_version\":01}",
        "{\"schema_version\":1,\"x\":\"\\q\"}",
        "{\"schema_version\":1,\"x\":\"\\uD800\"}",
        "{\"schema_version\":1,\"x\":\"\\uDC00\"}",
        encoded_empty() + "trailing",
    };
    for (const std::string& json : malformed) {
        const auto decoded = decode_workspace(json);
        CK_CHECK(!decoded);
        CK_CHECK(decoded.error.code == TodoCodecErrorCode::InvalidJson ||
                 decoded.error.code == TodoCodecErrorCode::TypeMismatch);
    }
    std::string invalid_utf8 = "{\"schema_version\":1,\"x\":\"";
    invalid_utf8.push_back(static_cast<char>(0xFF));
    invalid_utf8 += "\"}";
    const auto decoded = decode_workspace(invalid_utf8);
    CK_CHECK(!decoded && decoded.error.code == TodoCodecErrorCode::InvalidUtf8);
    CK_CHECK(decoded.error.byte_offset == 25);
}

CK_TEST(todo_codec_rejects_duplicate_missing_wrong_type_and_overflow_fields) {
    std::string json = replace_once(encoded_empty(),
                                    "\"schema_version\": 2",
                                    "\"schema_version\": 2,\n  \"schema_version\": 2");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::DuplicateField);

    json = replace_once(encoded_empty(), "  \"last_board_id\": 1,\n", "");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::MissingField);

    json = replace_once(encoded_empty(), "\"next_task_id\": 1", "\"next_task_id\": \"one\"");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::TypeMismatch);

    json = replace_once(encoded_empty(), "\"next_task_id\": 1",
                        "\"next_task_id\": 18446744073709551616");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::IntegerOutOfRange);
}

CK_TEST(todo_codec_future_schema_wins_over_v2_shape_validation) {
    const std::string json = R"json({
      "boards": 17,
      "schema_version": 3,
      "last_board_id": "not an id"
    })json";
    const auto decoded = decode_workspace(json);
    CK_CHECK(!decoded && decoded.error.code == TodoCodecErrorCode::UnsupportedSchemaVersion);
    CK_CHECK(decoded.error.path == "$.schema_version");
}

CK_TEST(todo_codec_reports_invalid_enums_dates_and_workspace_references) {
    std::string json = replace_once(encoded_empty(), "\"sort\": \"manual\"", "\"sort\": \"random\"");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::InvalidValue);

    TodoWorkspace workspace = TodoWorkspace::empty();
    TaskDraft task;
    task.title = "dated";
    task.due_date = IsoDate{"2026-08-31"};
    task.due_time = IsoTime{"14:30"};
    CK_CHECK(workspace.add_task(LaneId{1}, std::move(task), codec_stamp()));
    const std::string dated_json = *encode_workspace(workspace).value;
    CK_CHECK(dated_json.find("\"due_time\": \"14:30\"") != std::string::npos);
    json = replace_once(dated_json, "2026-08-31", "2026-02-30");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::InvalidWorkspace);

    json = replace_once(dated_json, "14:30", "24:00");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::InvalidWorkspace);

    json = replace_once(dated_json, "\"due_date\": \"2026-08-31\"", "\"due_date\": null");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::InvalidWorkspace);

    json = replace_once(dated_json, "\"task_ids\": [1]", "\"task_ids\": [99]");
    CK_CHECK(decode_error(json) == TodoCodecErrorCode::InvalidWorkspace);
}

CK_TEST(todo_codec_enforces_exact_string_input_and_nesting_limits) {
    std::string exact = replace_once(encoded_empty(), "\"title\": \"To Do\"",
                                     "\"title\": \"" + std::string(TodoLimits::max_name_bytes, 'x') + "\"");
    CK_CHECK(decode_workspace(exact));

    std::string oversized = replace_once(encoded_empty(), "\"title\": \"To Do\"",
                                         "\"title\": \"" +
                                             std::string(TodoLimits::max_name_bytes + 1, 'x') + "\"");
    CK_CHECK(decode_error(oversized) == TodoCodecErrorCode::StringTooLong);

    const auto nested_document = [](std::size_t arrays) {
        return std::string("{\"schema_version\":2,\"unknown\":") + std::string(arrays, '[') + "0" +
               std::string(arrays, ']') +
               ",\"last_board_id\":1,\"next_board_id\":2,\"next_lane_id\":4,\"next_task_id\":1," +
               "\"boards\":[{\"id\":1,\"name\":\"main\",\"lanes\":[" +
               "{\"id\":1,\"title\":\"To Do\",\"color\":null,\"sort\":\"manual\",\"task_ids\":[]}," +
               "{\"id\":2,\"title\":\"Doing\",\"color\":null,\"sort\":\"manual\",\"task_ids\":[]}," +
               "{\"id\":3,\"title\":\"Done\",\"color\":null,\"sort\":\"manual\",\"task_ids\":[]}" +
               "]}],\"tasks\":[]}";
    };
    CK_CHECK(decode_workspace(nested_document(31)));
    CK_CHECK(decode_error(nested_document(32)) == TodoCodecErrorCode::NestingTooDeep);
}

CK_TEST(todo_codec_rejects_input_above_the_document_byte_limit) {
    const std::string oversized(TodoCodecLimits::max_input_bytes + 1, ' ');
    const auto decoded = decode_workspace(oversized);
    CK_CHECK(!decoded && decoded.error.code == TodoCodecErrorCode::InputTooLarge);
    CK_CHECK(!decoded.value.has_value());
}
