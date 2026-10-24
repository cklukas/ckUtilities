// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_target_job.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>
namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;
using Op = FS::Operation;
template<class T> T take(Observation<T> observation) {
    if (!observation.value()) throw std::runtime_error("required source-target observation unavailable");
    return std::move(observation).take_value();
}
NativeName name(std::string text) { return *NativeName::from_posix(std::move(text)); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-source-target";
#else
    return "/simulated-source-target";
#endif
}
auto finish(auto& job) {
    for (int i = 0; i < 200; ++i) {
        (void)job.advance(); auto result = job.completed();
        if (result.value()) return std::move(result).take_value();
        if (job.failure()) throw std::runtime_error("source-target fixture job failed");
    }
    throw std::runtime_error("source-target fixture exhausted steps");
}
struct Fixture {
    FS fs;
    FS::Pointer source = fs.make_node(ObjectKind::Directory), destination = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
    std::shared_ptr<const DirectoryCursor> parent;
    HeldChild::Pointer held;
    Fixture() {
        fs.bind(path(), source); fs.bind(path() / "destination", destination); fs.put(source, name("a"), file);
        fs.metadata(file).logical_size = Observation<std::uint64_t>::available(3);
        parent = std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path()))); held = take(HeldChild::observe(parent, name("a")));
    }
    DestinationBatchJob::Result batch(std::vector<NativeName> names = {name("a")}, NameEncoding encoding = NameEncoding::PosixBytes) {
        DestinationBatchJob job(std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path() / "destination"))), std::move(names), encoding, {}, fs.clock());
        return finish(job);
    }
    void target(bool same) { fs.put(destination, name("a"), same ? file : fs.make_node(ObjectKind::RegularFile)); }
    SourceTargetJob::Result inspect(DestinationBatchJob::Result batch) {
        SourceTargetJob job({held}, std::move(batch), {}, fs.clock()); return finish(job);
    }
    std::size_t calls(Op op) const { return static_cast<std::size_t>(std::count_if(fs.calls().begin(), fs.calls().end(), [&](const auto& call) { return call.operation == op; })); }
};
}
CK_TEST(source_target_qualifies_same_held_object_across_independent_directories_and_keeps_both_owners) {
    Fixture f; f.target(true); auto batch = f.batch(); const auto before = f.calls(Op::InspectHandle), lookups = f.calls(Op::LookupChild);
    auto result = f.inspect(std::move(batch)); CK_CHECK(take(result->entries()[0].comparison) == IdentityComparison::Same);
    CK_CHECK(f.calls(Op::InspectHandle) == before + 2 && f.calls(Op::LookupChild) == lookups && f.fs.live_entries() == 2);
    CK_CHECK(result->sources()[0] == f.held && result->destination()->pin_directory());
}
CK_TEST(source_target_distinguishes_held_objects_without_claiming_content_or_name_difference) {
    Fixture f; f.target(false); const auto result = f.inspect(f.batch());
    CK_CHECK(take(result->entries()[0].comparison) == IdentityComparison::Different);
}
CK_TEST(source_target_missing_targets_are_not_comparable_and_never_become_different_objects) {
    Fixture f; auto batch = f.batch(); const auto before = f.calls(Op::InspectHandle); f.target(true);
    const auto result = f.inspect(std::move(batch)); CK_CHECK(!result->entries()[0].comparison.value());
    CK_CHECK(result->entries()[0].comparison.state() == ObservationState::Unsupported &&
        result->entries()[0].comparison.unavailable_detail()->error.operation == "source_target.no_existing_target");
    CK_CHECK(f.calls(Op::InspectHandle) == before + 1 && result->destination()->probe(0)->lookup().value()->state() == ChildLookupState::Missing);
}
CK_TEST(source_target_keeps_held_targets_after_both_original_names_are_replaced) {
    Fixture f; f.target(true); auto batch = f.batch(); f.fs.put(f.source, name("a"), f.fs.make_node(ObjectKind::RegularFile));
    f.fs.put(f.destination, name("a"), f.fs.make_node(ObjectKind::RegularFile));
    CK_CHECK(take(f.inspect(std::move(batch))->entries()[0].comparison) == IdentityComparison::Same);
}
CK_TEST(source_target_preserves_whole_query_errors_and_never_substitutes_cached_metadata) {
    for (auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed})
        for (bool target : {false, true}) {
            Fixture f; f.target(true); auto batch = f.batch(); if (target) f.fs.enqueue(Op::InspectHandle, {});
            const NativeError error{ErrorDomain::NtStatus, 77, "source_target.injected"};
            f.fs.enqueue(Op::InspectHandle, {UnavailableObservation{state, error}, {}, {}, false});
            const auto result = f.inspect(std::move(batch)); const auto& row = result->entries()[0];
            CK_CHECK(row.comparison.state() == state && row.comparison.unavailable_detail()->error == error);
            CK_CHECK((target ? row.target_now : row.source_now).state() == state);
        }
}
CK_TEST(source_target_missing_identity_or_kind_fields_keep_their_native_error_states) {
    for (bool target : {false, true}) for (bool kind : {false, true}) {
        Fixture f; auto other = f.fs.make_node(ObjectKind::RegularFile); f.fs.put(f.destination, name("a"), other); auto batch = f.batch();
        auto& metadata = f.fs.metadata(target ? other : f.file); const NativeError error{ErrorDomain::Posix, 13, "field.denied"};
        if (kind) metadata.kind = Observation<ObjectKind>::unavailable(ObservationState::Denied, error);
        else metadata.identity = Observation<ObjectIdentity>::unavailable(ObservationState::Denied, error);
        const auto result = f.inspect(std::move(batch)); CK_CHECK(result->entries()[0].comparison.state() == ObservationState::Denied && result->entries()[0].comparison.unavailable_detail()->error == error);
    }
}
CK_TEST(source_target_refuses_changed_reported_identity_and_object_kind_from_retained_handles) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f; auto other = f.fs.make_node(ObjectKind::RegularFile); f.fs.put(f.destination, name("a"), other); auto batch = f.batch();
        auto& metadata = f.fs.metadata(mode < 2 ? f.file : other);
        if (mode % 2 == 0) { auto id = take(metadata.identity); id.object = NativeIdentifier::from_u64(999); metadata.identity = Observation<ObjectIdentity>::available(id); }
        else metadata.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        CK_CHECK(f.inspect(std::move(batch))->entries()[0].comparison.state() == ObservationState::Changed);
    }
}
CK_TEST(source_target_scopes_schemes_zero_ids_and_generation_coverage_never_prove_difference) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f; auto other = f.fs.make_node(ObjectKind::RegularFile); f.fs.put(f.destination, name("a"), other); auto id = take(f.fs.metadata(f.file).identity);
        if (mode == 0) id.scope = AdapterScope{};
        if (mode == 1) id.scheme = IdentityScheme::WindowsFileId128;
        if (mode == 2) id.object = {};
        if (mode == 3) id.generation = 1;
        f.fs.metadata(other).identity = Observation<ObjectIdentity>::available(id);
        CK_CHECK(f.inspect(f.batch())->entries()[0].comparison.state() == ObservationState::Unknown);
    }
}
CK_TEST(source_target_rejects_same_ids_with_conflicting_stable_object_kind_observations) {
    Fixture f; auto other = f.fs.make_node(ObjectKind::Directory); f.fs.metadata(other).identity = f.fs.metadata(f.file).identity;
    f.fs.put(f.destination, name("a"), other);
    CK_CHECK(f.inspect(f.batch())->entries()[0].comparison.state() == ObservationState::Changed);
}
CK_TEST(source_target_changed_sizes_and_timestamps_do_not_turn_identity_into_content_equality) {
    Fixture f; f.target(true); auto batch = f.batch(); f.fs.metadata(f.file).logical_size = Observation<std::uint64_t>::available(900);
    f.fs.metadata(f.file).modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(42, 0));
    const auto result = f.inspect(std::move(batch)); CK_CHECK(take(result->entries()[0].comparison) == IdentityComparison::Same);
    CK_CHECK(take(result->entries()[0].source_now).logical_size.value() && take(take(result->entries()[0].source_now).logical_size) == 900);
}
CK_TEST(source_target_preserves_destination_lookup_and_mapping_failure_without_target_query) {
    for (bool mapping : {false, true}) {
        Fixture f; if (!mapping) f.fs.enqueue(Op::LookupChild, {UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "lookup.denied"}}, {}, {}, false});
        if (mapping) { const auto raw = name(std::string{"x\xff", 2}); f.fs.put(f.source, raw, f.file); f.held = take(HeldChild::observe(f.parent, raw)); }
        auto batch = f.batch({f.held->name()}, mapping ? NameEncoding::WindowsUtf16 : NameEncoding::PosixBytes);
        const auto before = f.calls(Op::InspectHandle); const auto result = f.inspect(std::move(batch));
        CK_CHECK(result->entries()[0].comparison.state() == (mapping ? ObservationState::Unsupported : ObservationState::Denied));
        CK_CHECK(f.calls(Op::InspectHandle) == before + 1);
    }
}
CK_TEST(source_target_cancellation_keeps_each_completed_query_private_and_does_not_repeat_it) {
    for (bool target : {false, true}) {
        Fixture f; f.target(true); auto batch = f.batch(); const auto before = f.calls(Op::InspectHandle); std::stop_source stop;
        if (target) f.fs.enqueue(Op::InspectHandle, {});
        f.fs.enqueue(Op::InspectHandle, {{}, {}, [&] { stop.request_stop(); }, false});
        SourceTargetJob job({f.held}, std::move(batch), {}, f.fs.clock());
        CK_CHECK(job.advance(stop.get_token()) == SourceTargetState::Paused && !job.completed().value());
        const auto count = f.calls(Op::InspectHandle); CK_CHECK(job.advance(stop.get_token()) == SourceTargetState::Paused && f.calls(Op::InspectHandle) == count);
        CK_CHECK(take(finish(job)->entries()[0].comparison) == IdentityComparison::Same && f.calls(Op::InspectHandle) == before + 2);
    }
}
CK_TEST(source_target_slow_queries_yield_without_repeating_native_work) {
    Fixture f; f.target(true); auto batch = f.batch(); const auto before = f.calls(Op::InspectHandle);
    f.fs.enqueue(Op::InspectHandle, {{}, std::chrono::milliseconds(20), {}, false}); SourceTargetJob job({f.held}, std::move(batch), {}, f.fs.clock());
    CK_CHECK(job.advance() == SourceTargetState::Collecting && !job.completed().value() && f.calls(Op::InspectHandle) == before + 1);
    CK_CHECK(take(finish(job)->entries()[0].comparison) == IdentityComparison::Same && f.calls(Op::InspectHandle) == before + 2);
}
CK_TEST(source_target_validates_all_source_rows_before_any_queries_and_never_rebinds_by_name) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f; auto batch = f.batch({name("a"), mode == 1 ? name("other") : name("a")}); const auto before = f.fs.calls().size();
        std::vector<HeldChild::Pointer> sources{f.held, mode == 0 ? HeldChild::Pointer{} : f.held}; SourceTargetLimits limits; if (mode == 2) limits.component_bytes = 1;
        if (mode == 2) { f.fs.put(f.source, name("long"), f.file); sources[1] = take(HeldChild::observe(f.parent, name("long"))); }
        const auto actual = f.fs.calls().size(); SourceTargetJob job(std::move(sources), std::move(batch), limits, f.fs.clock());
        CK_CHECK(job.advance() == SourceTargetState::Failed && f.fs.calls().size() == actual && !job.completed().value());
        CK_CHECK(actual >= before);
    }
}
CK_TEST(source_target_step_bound_stages_large_batches_and_publishes_only_a_complete_owned_result) {
    Fixture f; f.target(true); std::vector<NativeName> names(300, name("a")); DestinationBatchLimits batch_limits; batch_limits.rows = 300;
    DestinationBatchJob batch(std::shared_ptr<const DirectoryCursor>(take(f.fs.open_directory(path() / "destination"))), std::move(names), NameEncoding::PosixBytes, batch_limits, f.fs.clock());
    SourceTargetLimits limits; limits.rows = 300; SourceTargetJob job(std::vector<HeldChild::Pointer>(300, f.held), finish(batch), limits, f.fs.clock());
    const auto before = f.calls(Op::InspectHandle); CK_CHECK(job.advance() == SourceTargetState::Collecting && !job.completed().value() && f.calls(Op::InspectHandle) == before);
    const auto result = finish(job); CK_CHECK(result->entries().size() == 300 && f.calls(Op::InspectHandle) == before + 600);
    for (const auto& row : result->entries()) CK_CHECK(take(row.comparison) == IdentityComparison::Same);
}
CK_TEST(source_target_exceptions_and_clock_reversal_poison_and_release_private_state) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f; f.target(true); auto batch = f.batch(); SourceTargetJob job({f.held}, std::move(batch), {}, f.fs.clock());
        if (mode == 0) f.fs.enqueue(Op::InspectHandle, {{}, {}, {}, true});
        if (mode == 1) f.fs.enqueue(Op::InspectHandle, {{}, {}, [] { throw std::bad_alloc{}; }, false});
        if (mode == 2) { f.fs.enqueue(Op::InspectHandle, {{}, std::chrono::milliseconds(20), {}, false}); CK_CHECK(job.advance() == SourceTargetState::Collecting); f.fs.clock()->advance(-std::chrono::nanoseconds(1)); }
        bool threw = false; try { (void)job.advance(); } catch (const std::exception&) { threw = true; }
        CK_CHECK(threw == (mode != 2) && job.state() == SourceTargetState::Failed && !job.abandon() && f.fs.live_entries() == 1 && !job.completed().value());
    }
}
CK_TEST(source_target_issued_results_keep_owners_after_abandonment_and_last_release_retires_them) {
    Fixture f; f.target(true); SourceTargetJob job({f.held}, f.batch(), {}, f.fs.clock()); auto result = finish(job);
    f.held.reset(); f.parent.reset(); CK_CHECK(job.abandon() && !job.abandon() && !job.completed().value() && f.fs.live_entries() == 2);
    result.reset(); CK_CHECK(f.fs.live_entries() == 0 && f.fs.live_cursors() == 0);
}
CK_TEST(source_target_configuration_and_count_limits_are_checked_before_native_queries) {
    for (int mode = 0; mode < 9; ++mode) {
        Fixture f; auto batch = f.batch(); const auto before = f.fs.calls().size(); SourceTargetLimits limits;
        if (mode == 0) limits.rows = 0;
        if (mode == 1) limits.component_bytes = 0;
        if (mode == 2) limits.time_slice = {};
        if (mode == 3) limits.time_slice = std::chrono::seconds(2);
        if (mode == 7) limits.rows = DestinationBatchLimits::hard_rows + 1;
        if (mode == 8) limits.component_bytes = SourceTargetJob::hard_component_bytes + 1;
        std::vector<HeldChild::Pointer> sources{f.held};
        if (mode == 4) sources.clear();
        if (mode == 5) batch.reset();
        bool rejected = false; try { SourceTargetJob job(std::move(sources), std::move(batch), limits, mode == 6 ? std::shared_ptr<const MonotonicClock>{} : f.fs.clock()); }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected && f.fs.calls().size() == before);
    }
}
CK_TEST(source_target_unqualified_original_observations_do_not_gain_certainty_from_a_later_success) {
    for (bool target : {false, true}) {
        Fixture f; f.target(true); const auto original = f.fs.metadata(f.file).identity; const NativeError error{ErrorDomain::Posix, 13, "original.denied"};
        if (target) f.fs.enqueue(Op::InspectHandle, {UnavailableObservation{ObservationState::Denied, error}, {}, {}, false});
        else {
            f.fs.metadata(f.file).identity = Observation<ObjectIdentity>::unavailable(ObservationState::Denied, error);
            f.held = take(HeldChild::observe(f.parent, name("a"))); f.fs.metadata(f.file).identity = original;
        }
        const auto result = f.inspect(f.batch()); const auto& row = result->entries()[0];
        CK_CHECK(row.source_now.value() && row.target_now.value() && row.comparison.state() == ObservationState::Denied && row.comparison.unavailable_detail()->error == error);
    }
}
CK_TEST(source_target_utf16_component_limits_use_bytes_before_any_metadata_queries) {
    for (std::size_t bytes : {std::size_t{3}, std::size_t{4}}) {
        Fixture f; const auto wide = *NativeName::from_windows(u"ab"); f.fs.put(f.source, wide, f.file); f.fs.put(f.destination, wide, f.file);
        auto held = take(HeldChild::observe(f.parent, wide)); auto batch = f.batch({wide}, NameEncoding::WindowsUtf16); SourceTargetLimits limits; limits.component_bytes = bytes;
        const auto before = f.calls(Op::InspectHandle); SourceTargetJob job({held}, std::move(batch), limits, f.fs.clock());
        if (bytes == 3) CK_CHECK(job.advance() == SourceTargetState::Failed && f.calls(Op::InspectHandle) == before);
        else CK_CHECK(take(finish(job)->entries()[0].comparison) == IdentityComparison::Same && f.calls(Op::InspectHandle) == before + 2);
    }
}
CK_TEST(source_target_empty_batches_can_complete_and_row_limit_excess_is_explicit) {
    Fixture f; SourceTargetJob empty({}, f.batch({}), {}, f.fs.clock()); const auto before = f.fs.calls().size();
    CK_CHECK(finish(empty)->entries().empty() && f.fs.calls().size() == before);
    auto batch = f.batch({name("a"), name("a")}); SourceTargetLimits limits; limits.rows = 1;
    bool rejected = false; try { SourceTargetJob job({f.held, f.held}, std::move(batch), limits, f.fs.clock()); } catch (const std::length_error&) { rejected = true; }
    CK_CHECK(rejected);
}
CK_TEST(source_target_invalid_native_kind_is_failed_instead_of_qualified_identity_evidence) {
    Fixture f; f.target(true); auto batch = f.batch(); f.fs.metadata(f.file).kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(999));
    CK_CHECK(f.inspect(std::move(batch))->entries()[0].comparison.state() == ObservationState::Failed);
}
