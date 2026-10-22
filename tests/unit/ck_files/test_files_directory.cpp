// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"
#include "windows_directory_record.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <stdexcept>
#include <string>
#include <utility>

#include "cvision/testing/cktest.hpp"

namespace {
using namespace ck::files;
class ScriptedCursor final : public DirectoryCursor {
public:
    int calls = 0;
    int fail_at = -1;
    int throw_at = -1;
    int end_at = 5;
    bool skip_all = false;
    std::stop_source* cancel_after_name = nullptr;
private:
    Observation<DirectoryStep> next_step() override {
        using Result = Observation<DirectoryStep>;
        const int index = calls++;
        if (index == throw_at) throw std::runtime_error("injected adapter exception");
        if (index == fail_at) return Result::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "injected.read"});
        if (skip_all) return Result::available(DirectorySkip{});
        if (index == end_at) return Result::available(DirectoryEnd{});
        if (cancel_after_name) cancel_after_name->request_stop();
        return Result::available(*NativeName::from_posix(std::to_string(index)));
    }
};
}

CK_TEST(directory_batches_are_bounded_and_end_is_explicit_and_sticky) {
    ScriptedCursor cursor;
    const auto first = cursor.read(2);
    CK_CHECK(first.state == DirectoryReadState::More && first.names.size() == 2 && !first.error);
    CK_CHECK(*first.names[0].posix_bytes() == "0" && *first.names[1].posix_bytes() == "1");
    const auto second = cursor.read(3);
    CK_CHECK(second.state == DirectoryReadState::More && second.names.size() == 3);
    const auto last = cursor.read(1);
    CK_CHECK(last.state == DirectoryReadState::End && last.names.empty() && !last.error);
    const int calls = cursor.calls;
    CK_CHECK(cursor.read(1).state == DirectoryReadState::End && cursor.calls == calls);
}

CK_TEST(directory_partial_errors_retain_names_and_never_turn_into_end) {
    for (int index : {0, 2}) {
        ScriptedCursor cursor;
        cursor.fail_at = index;
        const auto batch = cursor.read(4);
        CK_CHECK(batch.names.size() == static_cast<std::size_t>(index));
        CK_CHECK(batch.state == DirectoryReadState::Error && batch.error);
        CK_CHECK(batch.error->state == ObservationState::Denied);
        CK_CHECK(batch.error->error == (NativeError{ErrorDomain::Posix, 13, "injected.read"}));
        const int calls = cursor.calls;
        const auto repeated = cursor.read(4);
        CK_CHECK(repeated.state == DirectoryReadState::Error && repeated.names.empty() && cursor.calls == calls);
    }
}

CK_TEST(directory_cancellation_before_and_during_a_batch_preserves_resume_position) {
    ScriptedCursor cursor;
    std::stop_source stopped;
    stopped.request_stop();
    const auto before = cursor.read(4, stopped.get_token());
    CK_CHECK(before.state == DirectoryReadState::Cancelled && before.names.empty() && !before.error && cursor.calls == 0);
    std::stop_source between;
    cursor.cancel_after_name = &between;
    const auto partial = cursor.read(4, between.get_token());
    CK_CHECK(partial.state == DirectoryReadState::Cancelled && partial.names.size() == 1 && !partial.error);
    cursor.cancel_after_name = nullptr;
    const auto resumed = cursor.read(4);
    CK_CHECK(resumed.names.size() == 4 && *resumed.names.front().posix_bytes() == "1");
    CK_CHECK(cursor.read(1).state == DirectoryReadState::End);
}

CK_TEST(directory_invalid_limits_do_not_consume_and_adapter_exceptions_poison_resume) {
    ScriptedCursor cursor;
    for (std::size_t invalid : {std::size_t{0}, DirectoryCursor::max_batch_size + 1}) {
        bool rejected = false;
        try { (void)cursor.read(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected && cursor.calls == 0);
    }
    cursor.throw_at = 1;
    bool threw = false;
    try { (void)cursor.read(4); }
    catch (const std::runtime_error&) { threw = true; }
    CK_CHECK(threw && cursor.calls == 2);
    const auto repeated = cursor.read(4);
    CK_CHECK(repeated.state == DirectoryReadState::Error && repeated.names.empty());
    CK_CHECK(repeated.error->state == ObservationState::Failed);
    CK_CHECK(repeated.error->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead));
    CK_CHECK(cursor.calls == 2);
}

// Synthetic native buffers test rejection paths that a conforming OS cannot
// normally produce. These do not substitute for actual Windows enumeration.
namespace {
void put_u32(std::vector<std::byte>& bytes, std::size_t position, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes.at(position + index) = static_cast<std::byte>(value & 0xFFU);
        value >>= 8U;
    }
}
std::vector<std::byte> directory_record(std::u16string_view name, std::size_t storage = 128) {
    std::vector<std::byte> bytes(storage);
    put_u32(bytes, detail::windows_directory_name_length_offset, static_cast<std::uint32_t>(name.size() * 2));
    for (std::size_t index = 0; index < name.size(); ++index) {
        bytes.at(detail::windows_directory_name_offset + index * 2) = static_cast<std::byte>(name[index] & 0xFFU);
        bytes.at(detail::windows_directory_name_offset + index * 2 + 1) = static_cast<std::byte>(name[index] >> 8U);
    }
    return bytes;
}
}

CK_TEST(windows_directory_records_use_counted_utf16_and_checked_next_offsets) {
    std::u16string spelling = u"A";
    spelling.push_back(static_cast<char16_t>(0xD800));
    auto bytes = directory_record(spelling, 256);
    put_u32(bytes, 0, 128);
    const auto decoded = detail::decode_windows_directory_record(bytes, 0);
    CK_CHECK(decoded.value() && decoded.value()->name && decoded.value()->next_offset == 128);
    CK_CHECK(*decoded.value()->name->windows_units() == spelling);
    for (auto navigation : {u".", u".."}) {
        const auto record = detail::decode_windows_directory_record(directory_record(navigation), 0);
        CK_CHECK(record.value() && !record.value()->name && !record.value()->next_offset);
    }
}

CK_TEST(windows_directory_records_refuse_truncated_overlapping_and_invalid_name_data) {
    for (std::size_t length = 0; length < detail::windows_directory_name_offset + 2; ++length)
        CK_CHECK(!detail::decode_windows_directory_record(std::vector<std::byte>(length), 0).value());
    const auto valid = directory_record(u"file");
    for (auto position : {std::size_t{1}, std::size_t{128}, std::numeric_limits<std::size_t>::max()})
        CK_CHECK(!detail::decode_windows_directory_record(valid, position).value());
    for (std::uint32_t length : {0U, 1U, 25U, 0xFFFFFFFFU}) {
        auto bytes = valid;
        put_u32(bytes, detail::windows_directory_name_length_offset, length);
        CK_CHECK(!detail::decode_windows_directory_record(bytes, 0).value());
    }
    for (std::uint32_t offset : {1U, 8U, 104U, 112U, 128U, 0xFFFFFFF8U}) {
        auto bytes = valid;
        put_u32(bytes, 0, offset);
        CK_CHECK(!detail::decode_windows_directory_record(bytes, 0).value());
    }
    for (const auto& name : {std::u16string{u'\0'}, std::u16string{u'a', u'/', u'b'}, std::u16string{u'a', u'\\', u'b'}})
        CK_CHECK(!detail::decode_windows_directory_record(directory_record(name), 0).value());
}

CK_TEST(directory_omitted_records_have_a_work_bound_and_are_cancellable) {
    ScriptedCursor cursor;
    cursor.skip_all = true;
    const auto batch = cursor.read(1);
    CK_CHECK(batch.state == DirectoryReadState::More && batch.names.empty());
    CK_CHECK(cursor.calls == static_cast<int>(DirectoryCursor::max_batch_size));
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = cursor.read(1, stop.get_token());
    CK_CHECK(cancelled.state == DirectoryReadState::Cancelled && cancelled.names.empty());
    CK_CHECK(cursor.calls == static_cast<int>(DirectoryCursor::max_batch_size));
}
