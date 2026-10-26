// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ancestor_walk_job.hpp"
#include "windows_parent_record.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>
namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;
using Op = FS::Operation;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required ancestor observation unavailable");
    return std::move(value).take_value();
}
NativeName name(std::string text) { return *NativeName::from_posix(std::move(text)); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-ancestor";
#else
    return "/simulated-ancestor";
#endif
}
auto finish(AncestorWalkJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == AncestorWalkState::Complete) return take(job.completed());
        if (job.state() == AncestorWalkState::Failed) throw std::runtime_error("ancestor fixture job failed");
    }
    throw std::runtime_error("ancestor fixture exceeded steps");
}
struct Fixture {
    FS fs;
    FS::Pointer root = fs.make_node(ObjectKind::Directory), source = fs.make_node(ObjectKind::Directory), inner = fs.make_node(ObjectKind::Directory), leaf = fs.make_node(ObjectKind::Directory);
    std::shared_ptr<const DirectoryCursor> root_cursor, destination;
    HeldChild::Pointer held;
    Fixture() {
        fs.bind(path(), root); fs.put(root, name("source"), source); fs.put(source, name("inner"), inner); fs.put(inner, name("leaf"), leaf);
        root_cursor = std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path()))); held = take(HeldChild::observe(root_cursor, name("source")));
        auto first = take(root_cursor->open_directory_child(name("source"))); auto second = take(first->open_directory_child(name("inner")));
        destination = std::shared_ptr<const DirectoryCursor>(take(second->open_directory_child(name("leaf"))));
    }
    std::size_t calls(Op op) const { return static_cast<std::size_t>(std::count_if(fs.calls().begin(), fs.calls().end(), [&](const auto& c) { return c.operation == op; })); }
};
void put(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value, std::size_t count = 4) {
    for (std::size_t i = 0; i < count; ++i) bytes[at + i] = std::byte((value >> (i * 8U)) & 255U);
}
std::vector<std::byte> record(unsigned major) {
    const std::size_t header = major == 3 ? 76 : 60, id_size = major == 3 ? 16 : 8;
    std::vector<std::byte> bytes(header + 4);
    put(bytes, 0, static_cast<std::uint32_t>(bytes.size())); put(bytes, 4, major, 2);
    bytes[8] = std::byte{9}; bytes[8 + id_size] = std::byte{7};
    put(bytes, header - 8, 16); put(bytes, header - 4, 2, 2); put(bytes, header - 2, static_cast<std::uint32_t>(header), 2); put(bytes, header, 'x', 2);
    return bytes;
}
}
CK_TEST(ancestor_walk_observes_source_and_keeps_every_directory_owner_without_path_lookups) {
    Fixture f; const auto calls = f.calls(Op::OpenDirectory); AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock()); const auto result = finish(job);
    const auto end = take(result->outcome()); CK_CHECK(end.end == AncestorWalkEnd::ObservedSource && end.directory == 2 && !end.repeated);
    CK_CHECK(result->directories().size() == 3 && f.calls(Op::OpenParent) == 2 && f.calls(Op::OpenDirectory) == calls);
    CK_CHECK(result->directories()[1].owner && result->directories()[2].owner && !result->directories()[0].owner);
}
CK_TEST(ancestor_walk_self_match_needs_no_parent_open_and_exact_depth_limits_allow_a_final_match) {
    for (std::size_t count : {std::size_t{2}, std::size_t{3}}) {
        Fixture f; AncestorWalkLimits limits; limits.directories = count; AncestorWalkJob job(f.held, f.destination, limits, f.fs.clock()); auto result = finish(job);
        if (count == 2) CK_CHECK(result->outcome().state() == ObservationState::Unknown && result->outcome().unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
        else CK_CHECK(take(result->outcome()).end == AncestorWalkEnd::ObservedSource);
        CK_CHECK(f.calls(Op::OpenParent) == count - 1);
    }
    Fixture f; auto cursor = std::shared_ptr<const DirectoryCursor>(take(f.root_cursor->open_directory_child(name("source"))));
    AncestorWalkJob job(f.held, std::move(cursor), {1}, f.fs.clock()); CK_CHECK(take(finish(job)->outcome()).directory == 0 && f.calls(Op::OpenParent) == 0);
}
CK_TEST(ancestor_walk_self_parent_root_is_a_disjoint_boundary_witness) {
    Fixture f; AncestorWalkJob job(f.held, f.root_cursor, {}, f.fs.clock()); const auto result = finish(job); const auto end = take(result->outcome());
    CK_CHECK(end.end == AncestorWalkEnd::DisjointBoundary && end.directory == 1 && end.repeated == 0 && result->directories().size() == 2);
}
CK_TEST(ancestor_walk_preserves_each_parent_query_failure_with_an_explicit_bounded_prefix) {
    for (auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        Fixture f; const NativeError error{ErrorDomain::Win32, 5, "parent.injected"}; f.fs.enqueue(Op::OpenParent, {UnavailableObservation{state, error}, {}, {}, false});
        AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock()); auto result = finish(job);
        CK_CHECK(result->directories().size() == 1 && result->outcome().state() == state && result->outcome().unavailable_detail()->error == error);
    }
}
CK_TEST(ancestor_walk_preserves_metadata_failures_at_source_destination_and_parent_queries) {
    for (int stage = 0; stage < 3; ++stage) {
        Fixture f; for (int i = 0; i < stage; ++i) f.fs.enqueue(Op::InspectHandle, {});
        f.fs.enqueue(Op::InspectHandle, {UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "metadata.denied"}}, {}, {}, false});
        AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock()); const auto result = finish(job);
        CK_CHECK(result->outcome().state() == ObservationState::Denied && result->directories().size() == static_cast<std::size_t>(stage));
    }
}
CK_TEST(ancestor_walk_rejects_unknown_or_changed_source_identity_before_destination_traversal) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f;
        if (mode == 0) f.fs.metadata(f.source).identity = {};
        if (mode == 1) { auto id = take(f.fs.metadata(f.source).identity); id.object = NativeIdentifier::from_u64(999); f.fs.metadata(f.source).identity = Observation<ObjectIdentity>::available(id); }
        if (mode == 2) f.fs.metadata(f.source).kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        if (mode == 3) { auto id = take(f.fs.metadata(f.source).identity); id.scope = AdapterScope{}; f.fs.metadata(f.source).identity = Observation<ObjectIdentity>::available(id); }
        AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock()); const auto result = finish(job);
        CK_CHECK(result->outcome().state() == (mode == 1 || mode == 2 ? ObservationState::Changed : ObservationState::Unknown) && result->directories().empty() && f.calls(Op::OpenParent) == 0);
    }
}
CK_TEST(ancestor_walk_cancellation_commits_each_completed_query_or_parent_open_before_yielding) {
    for (bool parent : {false, true}) {
        Fixture f; const auto before = f.calls(Op::InspectHandle); std::stop_source stop; f.fs.enqueue(parent ? Op::OpenParent : Op::InspectHandle, {{}, {}, [&] { stop.request_stop(); }, false});
        AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock()); CK_CHECK(job.advance(stop.get_token()) == AncestorWalkState::Paused && !job.completed().value());
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance(stop.get_token()) == AncestorWalkState::Paused && f.fs.calls().size() == calls);
        CK_CHECK(take(finish(job)->outcome()).end == AncestorWalkEnd::ObservedSource && f.calls(Op::OpenParent) == 2 && f.calls(Op::InspectHandle) == before + 4);
    }
}
CK_TEST(ancestor_walk_slow_parent_queries_keep_the_pending_owner_for_resume_and_release_on_abandon) {
    Fixture f; f.fs.enqueue(Op::OpenParent, {{}, std::chrono::milliseconds(20), {}, false}); AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock());
    CK_CHECK(job.advance() == AncestorWalkState::Collecting && f.fs.live_entries() == 2 && !job.completed().value());
    CK_CHECK(job.abandon() && !job.abandon() && f.fs.live_entries() == 1 && job.advance() == AncestorWalkState::Abandoned);
}
CK_TEST(ancestor_walk_issued_results_hold_source_destination_and_parents_until_worker_retirement) {
    Fixture f; AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock()); auto result = finish(job);
    f.held.reset(); f.destination.reset(); f.root_cursor.reset(); CK_CHECK(job.abandon() && f.fs.live_entries() == 3);
    result.reset(); CK_CHECK(f.fs.live_entries() == 0 && f.fs.live_cursors() == 0);
}
CK_TEST(ancestor_walk_exceptions_and_clock_reversal_poison_and_release_staging) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f; AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock());
        if (mode == 0) f.fs.enqueue(Op::OpenParent, {{}, {}, {}, true});
        if (mode == 1) f.fs.enqueue(Op::InspectHandle, {{}, {}, [] { throw std::bad_alloc{}; }, false});
        if (mode == 2) { f.fs.enqueue(Op::OpenParent, {{}, std::chrono::milliseconds(20), {}, false}); CK_CHECK(job.advance() == AncestorWalkState::Collecting); f.fs.clock()->advance(-std::chrono::nanoseconds(1)); }
        bool threw = false; try { (void)job.advance(); } catch (const std::exception&) { threw = true; }
        CK_CHECK(threw == (mode != 2) && job.state() == AncestorWalkState::Failed && !job.completed().value() && !job.abandon() && f.fs.live_entries() == 1);
    }
}
CK_TEST(ancestor_walk_configuration_is_validated_before_queries) {
    for (int mode = 0; mode < 7; ++mode) {
        Fixture f; AncestorWalkLimits limits; if (mode == 0) limits.directories = 0;
        if (mode == 1) limits.directories = AncestorWalkJob::hard_directories + 1;
        if (mode == 2) limits.time_slice = {};
        if (mode == 3) limits.time_slice = std::chrono::seconds(2);
        bool rejected = false; const auto calls = f.fs.calls().size();
        try { AncestorWalkJob job(mode == 4 ? HeldChild::Pointer{} : f.held, mode == 5 ? std::shared_ptr<const DirectoryCursor>{} : f.destination,
            limits, mode == 6 ? std::shared_ptr<const MonotonicClock>{} : f.fs.clock()); } catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected && f.fs.calls().size() == calls);
    }
}
CK_TEST(windows_parent_records_keep_exact_64_and_128_bit_identifiers_without_names_or_casts) {
    for (unsigned major : {2U, 3U}) {
        const auto parsed = take(detail::decode_windows_parent_record(record(major), major));
        CK_CHECK(parsed.scheme == (major == 3 ? IdentityScheme::WindowsFileId128 : IdentityScheme::WindowsFileIndex64));
        CK_CHECK(parsed.child == NativeIdentifier::from_u64(9) && parsed.parent == NativeIdentifier::from_u64(7) && parsed.attributes == 16);
    }
}
CK_TEST(windows_parent_records_reject_truncation_length_version_alignment_and_name_bounds) {
    for (unsigned major : {2U, 3U}) {
        auto valid = record(major);
        for (std::size_t i = 0; i < valid.size(); ++i) CK_CHECK(!detail::decode_windows_parent_record(std::span<const std::byte>(valid).first(i), major).value());
        for (int mode = 0; mode < 7; ++mode) {
            auto bytes = valid; const auto header = major == 3 ? std::size_t{76} : std::size_t{60};
            if (mode == 0) put(bytes, 0, 0);
            if (mode == 1) put(bytes, 4, major == 3 ? 2 : 3, 2);
            if (mode == 2) put(bytes, 6, 1, 2);
            if (mode == 3) put(bytes, header - 4, 3, 2);
            if (mode == 4) put(bytes, header - 2, static_cast<std::uint32_t>(header - 1), 2);
            if (mode == 5) put(bytes, header - 2, 65534, 2);
            if (mode == 6) put(bytes, header - 4, 65534, 2);
            CK_CHECK(detail::decode_windows_parent_record(bytes, major).state() == ObservationState::Failed);
        }
    }
}
CK_TEST(windows_parent_records_zero_ids_are_unknown_and_empty_names_remain_valid_native_metadata) {
    for (unsigned major : {2U, 3U}) {
        for (bool parent : {false, true}) {
            auto bytes = record(major); bytes[parent ? (major == 3 ? 24 : 16) : 8] = std::byte{};
            CK_CHECK(detail::decode_windows_parent_record(bytes, major).state() == ObservationState::Unknown);
        }
        auto bytes = record(major); put(bytes, (major == 3 ? 76 : 60) - 4, 0, 2); CK_CHECK(detail::decode_windows_parent_record(bytes, major).value());
    }
    CK_CHECK(detail::decode_windows_parent_record(record(3), 4).state() == ObservationState::Failed);
}
CK_TEST(ancestor_walk_long_walks_yield_during_repeat_comparisons_without_exposing_a_partial_result) {
    Fixture f; auto leaf = f.fs.make_node(ObjectKind::Directory); f.fs.put(f.root, name("outside"), leaf);
    for (int i = 0; i < 40; ++i) { auto next = f.fs.make_node(ObjectKind::Directory); f.fs.put(leaf, name("child"), next); leaf = std::move(next); }
    f.fs.bind(path() / "leaf-alias", leaf); auto destination = std::shared_ptr<const DirectoryCursor>(take(f.fs.open_directory(path() / "leaf-alias")));
    AncestorWalkJob job(f.held, std::move(destination), {}, f.fs.clock());
    CK_CHECK(job.advance() == AncestorWalkState::Collecting && !job.completed().value());
    const auto result = finish(job); const auto end = take(result->outcome());
    CK_CHECK(end.end == AncestorWalkEnd::RepeatedDirectory && end.directory == 42 && end.repeated == 41 && result->directories().size() == 43 && f.calls(Op::OpenParent) == 42);
}
CK_TEST(ancestor_walk_metadata_identity_changes_at_the_starting_cursor_do_not_trigger_parent_opens) {
    Fixture f; auto id = take(f.fs.metadata(f.leaf).identity); id.object = NativeIdentifier::from_u64(999); f.fs.metadata(f.leaf).identity = Observation<ObjectIdentity>::available(id);
    AncestorWalkJob job(f.held, f.destination, {}, f.fs.clock());
    CK_CHECK(finish(job)->outcome().state() == ObservationState::Changed && f.calls(Op::OpenParent) == 0);
}
CK_TEST(ancestor_walk_malformed_null_parent_replies_poison_instead_of_issuing_an_incomplete_report) {
    struct BrokenCursor final : DirectoryCursor {
        explicit BrokenCursor(BasicMetadata metadata) : DirectoryCursor(metadata.identity), metadata_(std::move(metadata)) {}
        Observation<BasicMetadata> inspect_basic() const override { return Observation<BasicMetadata>::available(metadata_); }
        Observation<std::unique_ptr<OpenedEntry>> open_parent_directory() const override { return Observation<std::unique_ptr<OpenedEntry>>::available({}); }
        Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
        BasicMetadata metadata_;
    };
    Fixture f; AncestorWalkJob job(f.held, std::make_shared<BrokenCursor>(f.fs.metadata(f.leaf)), {}, f.fs.clock());
    CK_CHECK(job.advance() == AncestorWalkState::Failed && !job.completed().value() && job.failure()->error.code == static_cast<std::int64_t>(AdapterError::InvalidNativeResult));
}
CK_TEST(windows_parent_records_preserve_high_identifier_bytes_and_never_downgrade_to_64_bits) {
    auto bytes = record(3); bytes[23] = std::byte{0x81}; bytes[39] = std::byte{0x93};
    const auto parsed = take(detail::decode_windows_parent_record(bytes, 3));
    CK_CHECK(parsed.child.bytes[15] == std::byte{0x81} && parsed.parent.bytes[15] == std::byte{0x93});
    CK_CHECK(detail::decode_windows_parent_record(bytes, 2).state() == ObservationState::Failed);
}
CK_TEST(simulated_parent_tracking_retains_a_shared_parent_until_the_last_exact_entry_is_removed) {
    Fixture f; f.fs.put(f.root, name("alias"), f.source); auto entry = take(f.root_cursor->open_child(name("source")));
    f.fs.erase(f.root, name("source")); auto parent = take(entry->open_parent_directory());
    CK_CHECK(compare_identity(take(take(parent->inspect_basic()).identity), take(f.root_cursor->identity())) == IdentityComparison::Same);
    f.fs.put(f.root, name("source"), f.source); f.fs.put(f.root, name("alias"), f.fs.make_node(ObjectKind::Directory));
    parent = take(entry->open_parent_directory()); CK_CHECK(compare_identity(take(take(parent->inspect_basic()).identity), take(f.root_cursor->identity())) == IdentityComparison::Same);
    f.fs.erase(f.root, name("source")); CK_CHECK(entry->open_parent_directory().state() == ObservationState::Gone);
}
