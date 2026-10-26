// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_batch_job.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <new>

namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;
using Op = FS::Operation;
using State = DestinationBatchState;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required destination batch fixture unavailable");
    return std::move(value).take_value();
}
NativeName name(std::string text) { return *NativeName::from_posix(std::move(text)); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-destination-batch";
#else
    return "/simulated-destination-batch";
#endif
}
struct Fixture {
    FS fs;
    FS::Pointer root = fs.make_node(ObjectKind::Directory);
    Fixture() { fs.bind(path(), root); }
    std::shared_ptr<const DirectoryCursor> parent() { return std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path()))); }
    FS::Pointer put(const NativeName& component) {
        auto file = fs.make_node(ObjectKind::RegularFile); fs.put(root, component, file); return file;
    }
    std::size_t calls(Op op) const {
        return static_cast<std::size_t>(std::count_if(fs.calls().begin(), fs.calls().end(), [&](const auto& call) { return call.operation == op; }));
    }
};
DestinationBatchJob::Result finish(DestinationBatchJob& job) {
    for (int i = 0; i < 200; ++i) {
        const auto state = job.advance();
        if (state == State::Complete) return take(job.completed());
        if (state == State::Failed) throw std::runtime_error("destination batch unexpectedly failed");
    }
    throw std::runtime_error("destination batch fixture step bound exceeded");
}
}
CK_TEST(destination_batch_groups_exact_mapped_names_before_probing_and_preserves_each_source) {
    Fixture f; (void)f.put(name("same")); (void)f.put(name("other"));
    const auto windows = *NativeName::from_windows(u"same");
    DestinationBatchJob job(f.parent(), {name("same"), windows, name("other")}, NameEncoding::PosixBytes, {}, f.fs.clock());
    const auto result = finish(job);
    CK_CHECK(result->entries().size() == 3 && result->entries()[1].source == windows);
    CK_CHECK(result->entries()[0].exact_name_leader == 0 && result->entries()[1].exact_name_leader == 0 && result->entries()[2].exact_name_leader == 2);
    CK_CHECK(result->exact_name_count(0) == 2 && result->exact_name_count(1) == 2 && result->exact_name_count(2) == 1);
    CK_CHECK(result->probe(0) && result->probe(0) == result->probe(1) && result->probe(0) != result->probe(2));
    CK_CHECK(f.calls(Op::LookupChild) == 2 && f.fs.live_entries() == 2 && f.fs.live_cursors() == 1);
}
CK_TEST(destination_batch_scopes_exact_name_groups_to_their_parent_cursor) {
    FS fs;
    auto first = fs.make_node(ObjectKind::Directory), second = fs.make_node(ObjectKind::Directory);
    fs.bind("/destination-batch-scope-first", first);
    fs.bind("/destination-batch-scope-second", second);
    fs.put(first, name("same"), fs.make_node(ObjectKind::RegularFile));
    fs.put(second, name("same"), fs.make_node(ObjectKind::RegularFile));
    std::shared_ptr<const DirectoryCursor> first_cursor(take(fs.open_directory("/destination-batch-scope-first")));
    std::shared_ptr<const DirectoryCursor> second_cursor(take(fs.open_directory("/destination-batch-scope-second")));
    DestinationBatchJob job({first_cursor, second_cursor}, {name("same"), name("same")},
        NameEncoding::PosixBytes, {}, fs.clock());
    const auto result = finish(job);
    CK_CHECK(result->entries().size() == 2);
    CK_CHECK(result->exact_name_count(0) == 1 && result->exact_name_count(1) == 1);
    CK_CHECK(result->entries()[0].exact_name_leader == 0 && result->entries()[1].exact_name_leader == 1);
    CK_CHECK(result->probe(0) && result->probe(1) && result->probe(0) != result->probe(1));
}
CK_TEST(destination_batch_reports_hard_link_object_matches_separately_from_exact_name_groups) {
    Fixture f; auto object = f.put(name("a")); f.fs.put(f.root, name("b"), object);
    DestinationBatchJob job(f.parent(), {name("a"), name("b"), name("b")}, NameEncoding::PosixBytes, {}, f.fs.clock());
    const auto result = finish(job);
    CK_CHECK(result->exact_name_count(0) == 1 && result->exact_name_count(1) == 2);
    CK_CHECK(result->reported_object_leader(0) == 0 && result->reported_object_leader(1) == 0 && result->reported_object_leader(2) == 0);
    CK_CHECK(result->entries()[0].exact_name_leader != result->entries()[1].exact_name_leader && f.calls(Op::LookupChild) == 2);
}
CK_TEST(destination_batch_does_not_infer_case_or_normalization_equivalence_from_directory_advertisements) {
    Fixture f; LocationInfo location; location.case_sensitive_directory = Observation<bool>::available(false);
    f.fs.set_location(f.root, Observation<LocationInfo>::available(location));
    const std::vector<NativeName> names{name("A"), name("a"), name("\xc3\xa9"), name("e\xcc\x81")};
    DestinationBatchJob job(f.parent(), names, NameEncoding::PosixBytes, {}, f.fs.clock()); const auto result = finish(job);
    for (std::size_t i = 0; i < names.size(); ++i) {
        CK_CHECK(take(result->entries()[i].mapped) == names[i] && result->exact_name_count(i) == 1);
        CK_CHECK(result->probe(i)->lookup().value()->state() == ChildLookupState::Missing && !result->reported_object_leader(i));
    }
    CK_CHECK(f.calls(Op::LookupChild) == 4);
}
CK_TEST(destination_batch_uses_scope_scheme_generation_and_nonzero_identity_qualification) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f; auto first = f.put(name("a")), second = f.put(name("b"));
        auto identity = take(f.fs.metadata(first).identity);
        if (mode == 0) identity.scope = AdapterScope{};
        if (mode == 1) identity.generation = 9;
        if (mode == 2) identity.scheme = IdentityScheme::WindowsFileId128;
        if (mode == 3) { identity.object = {}; f.fs.metadata(first).identity = Observation<ObjectIdentity>::available(identity); }
        f.fs.metadata(second).identity = Observation<ObjectIdentity>::available(identity);
        DestinationBatchJob job(f.parent(), {name("a"), name("b")}, NameEncoding::PosixBytes, {}, f.fs.clock()); const auto result = finish(job);
        CK_CHECK(!result->reported_object_leader(0) && !result->reported_object_leader(1));
    }
}
CK_TEST(destination_batch_unrepresentable_names_keep_original_units_and_never_reach_native_lookup) {
    for (bool windows : {false, true}) {
        Fixture f;
        const auto raw = windows ? *NativeName::from_windows(std::u16string{u'x', char16_t{0xd800}}) : name(std::string{"x\xff", 2});
        DestinationBatchJob job(f.parent(), {raw}, windows ? NameEncoding::PosixBytes : NameEncoding::WindowsUtf16, {}, f.fs.clock());
        const auto result = finish(job);
        CK_CHECK(result->entries()[0].source == raw && result->entries()[0].mapped.state() == ObservationState::Unsupported);
        CK_CHECK(!result->probe(0) && !result->probe_failure(0) && result->exact_name_count(0) == 0 && f.calls(Op::LookupChild) == 0);
    }
}
CK_TEST(destination_batch_same_encoding_preserves_raw_names_without_display_or_unicode_roundtrips) {
    for (bool windows : {false, true}) {
        Fixture f;
        const auto raw = windows ? *NativeName::from_windows(std::u16string{u'x', char16_t{0xd800}}) : name(std::string{"x\xff", 2});
        (void)f.put(raw); DestinationBatchJob job(f.parent(), {raw}, raw.encoding(), {}, f.fs.clock());
        const auto result = finish(job); CK_CHECK(take(result->entries()[0].mapped) == raw);
        CK_CHECK(result->probe(0)->lookup().value()->state() == ChildLookupState::Found && f.calls(Op::LookupChild) == 1);
    }
}
CK_TEST(destination_batch_keeps_each_native_failure_state_and_error_in_the_completed_probe) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
        ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        Fixture f; const NativeError error{ErrorDomain::NtStatus, 73, "batch.injected"};
        for (const auto op : {Op::InspectLocation, Op::LookupChild}) f.fs.enqueue(op, {UnavailableObservation{state, error}, {}, {}, false});
        DestinationBatchJob job(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock()); const auto result = finish(job);
        CK_CHECK(result->probe(0) && !result->probe_failure(0) && result->probe(0)->lookup().state() == state);
        CK_CHECK(result->probe(0)->lookup().unavailable_detail()->error == error && result->probe(0)->location().state() == state);
        CK_CHECK(!result->reported_object_leader(0));
    }
}
CK_TEST(destination_batch_unreadable_existing_metadata_is_not_a_missing_target_or_an_alias_match) {
    Fixture f; (void)f.put(name("a"));
    f.fs.enqueue(Op::InspectHandle, {UnavailableObservation{ObservationState::Denied, {ErrorDomain::Adapter, 4, "batch.inspect"}}, {}, {}, false});
    DestinationBatchJob job(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(result->probe(0)->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(result->probe(0)->existing_metadata().state() == ObservationState::Denied && !result->reported_object_leader(0) && f.fs.live_entries() == 1);
}
CK_TEST(destination_batch_cancellation_drops_only_the_incomplete_probe_and_resumes_without_repeating_prior_names) {
    for (const auto op : {Op::InspectLocation, Op::LookupChild, Op::InspectHandle}) {
        Fixture f; (void)f.put(name("a")); (void)f.put(name("b")); std::stop_source stop;
        f.fs.enqueue(op, {}); f.fs.enqueue(op, {{}, {}, [&] { stop.request_stop(); }, false});
        DestinationBatchJob job(f.parent(), {name("a"), name("b")}, NameEncoding::PosixBytes, {}, f.fs.clock());
        CK_CHECK(job.advance(stop.get_token()) == State::Paused && !job.completed().value() && f.fs.live_entries() == 1);
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance(stop.get_token()) == State::Paused && f.fs.calls().size() == calls);
        const auto result = finish(job); CK_CHECK(result->probe(0) && result->probe(1) && f.fs.live_entries() == 2);
        const auto first_queries = std::count_if(f.fs.calls().begin(), f.fs.calls().end(), [](const auto& call) {
            return call.operation == Op::LookupChild && call.name == name("a");
        });
        CK_CHECK(first_queries == 1);
    }
}
CK_TEST(destination_batch_slow_complete_probe_is_committed_before_time_yield_without_repeating_native_work) {
    Fixture f; (void)f.put(name("a")); (void)f.put(name("b"));
    f.fs.enqueue(Op::InspectLocation, {{}, std::chrono::milliseconds(25), {}, false});
    DestinationBatchJob job(f.parent(), {name("a"), name("b")}, NameEncoding::PosixBytes, {}, f.fs.clock());
    CK_CHECK(job.advance() == State::Collecting && f.calls(Op::LookupChild) == 1 && !job.completed().value());
    CK_CHECK(finish(job)->entries().size() == 2 && f.calls(Op::LookupChild) == 2);
}
CK_TEST(destination_batch_large_missing_batches_are_step_bounded_and_never_publish_partial_records) {
    Fixture f; std::vector<NativeName> names;
    for (int i = 0; i < 300; ++i) names.push_back(name("entry-" + std::to_string(i)));
    DestinationBatchLimits limits; limits.rows = 300; DestinationBatchJob job(f.parent(), std::move(names), NameEncoding::PosixBytes, limits, f.fs.clock());
    int advances = 0;
    while (job.state() != State::Complete && advances < 20) {
        const auto calls = f.calls(Op::LookupChild); const auto state = job.advance();
        CK_CHECK(state != State::Failed && f.calls(Op::LookupChild) - calls <= DestinationBatchJob::steps_per_advance);
        if (state != State::Complete) CK_CHECK(!job.completed().value());
        ++advances;
    }
    CK_CHECK(advances >= 4 && job.state() == State::Complete && take(job.completed())->entries().size() == 300);
    CK_CHECK(f.calls(Op::LookupChild) == 300 && f.fs.live_entries() == 0);
}
CK_TEST(destination_batch_identity_bucket_collisions_remain_cooperative_and_never_merge_foreign_scopes) {
    Fixture f; std::vector<NativeName> names;
    for (int i = 0; i < 64; ++i) {
        auto component = name(std::to_string(i)); auto file = f.put(component); names.push_back(component);
        f.fs.metadata(file).identity = Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::PosixDeviceInode,
            NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(9), {}});
    }
    DestinationBatchJob job(f.parent(), std::move(names), NameEncoding::PosixBytes, {}, f.fs.clock());
    int advances = 0; while (job.state() != State::Complete && advances < 100) { (void)job.advance(); ++advances; }
    const auto result = take(job.completed()); CK_CHECK(advances > 8 && f.calls(Op::LookupChild) == 64);
    for (std::size_t i = 0; i < result->entries().size(); ++i) CK_CHECK(!result->reported_object_leader(i));
}
CK_TEST(destination_batch_name_budget_reserves_probe_contexts_once_and_fails_before_native_queries) {
    for (std::size_t budget : {std::size_t{4}, std::size_t{5}, std::size_t{6}, std::size_t{7}}) {
        Fixture f; DestinationBatchLimits limits; limits.name_bytes = budget;
        const bool duplicate = budget >= 6;
        DestinationBatchJob job(f.parent(), duplicate ? std::vector<NativeName>{name("a"), name("a")} : std::vector<NativeName>{name("a")},
            NameEncoding::PosixBytes, limits, f.fs.clock());
        (void)job.advance();
        if (budget == 4 || budget == 6) CK_CHECK(job.state() == State::Failed && f.calls(Op::LookupChild) == 0 && f.fs.live_cursors() == 0);
        else CK_CHECK(finish(job)->entries().size() == (duplicate ? 2 : 1) && f.calls(Op::LookupChild) == 1);
    }
}
CK_TEST(destination_batch_row_and_component_limits_fail_without_returning_a_prefix) {
    for (bool rows : {false, true}) {
        Fixture f; DestinationBatchLimits limits;
        if (rows) limits.rows = 1; else limits.component_bytes = 1;
        DestinationBatchJob job(f.parent(), rows ? std::vector<NativeName>{name("a"), name("b")} : std::vector<NativeName>{name("aa")},
            NameEncoding::PosixBytes, limits, f.fs.clock());
        CK_CHECK(job.advance() == State::Failed && !job.completed().value() && f.calls(Op::LookupChild) == 0 && f.fs.live_cursors() == 0);
    }
}
CK_TEST(destination_batch_exceptions_poison_and_close_all_private_native_owners_with_nonallocating_text) {
    for (const auto op : {Op::InspectLocation, Op::LookupChild, Op::InspectHandle}) {
        Fixture f; (void)f.put(name("a")); f.fs.enqueue(op, {{}, {}, [] { throw std::bad_alloc{}; }, false});
        DestinationBatchJob job(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock()); bool caught = false;
        try { (void)job.advance(); } catch (const std::bad_alloc&) { caught = true; }
        CK_CHECK(caught && job.state() == State::Failed && job.failure()->error.operation.empty() && f.fs.live_entries() == 0 && f.fs.live_cursors() == 0);
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance() == State::Failed && f.fs.calls().size() == calls && !job.abandon());
    }
}
CK_TEST(destination_batch_clock_reversal_releases_staged_probes_and_prevents_retry) {
    Fixture f; (void)f.put(name("a")); f.fs.enqueue(Op::InspectLocation, {{}, std::chrono::milliseconds(25), {}, false});
    DestinationBatchJob job(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock()); CK_CHECK(job.advance() == State::Collecting && f.fs.live_entries() == 1);
    f.fs.clock()->value = MonotonicClock::TimePoint{} - std::chrono::seconds(1);
    CK_CHECK(job.advance() == State::Failed && !job.completed().value() && f.fs.live_entries() == 0 && f.fs.live_cursors() == 0);
}
CK_TEST(destination_batch_abandonment_preserves_issued_immutable_results_until_last_worker_owner) {
    Fixture f; (void)f.put(name("a")); DestinationBatchJob job(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock());
    auto result = finish(job); const auto calls = f.fs.calls().size();
    CK_CHECK(job.advance() == State::Complete && f.fs.calls().size() == calls && job.abandon());
    CK_CHECK(f.fs.live_entries() == 1 && f.fs.live_cursors() == 1 && result->probe(0));
    result.reset(); CK_CHECK(f.fs.live_entries() == 0 && f.fs.live_cursors() == 0 && !job.abandon());
    DestinationBatchJob paused(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock()); std::stop_source stop; stop.request_stop();
    const auto prior = f.fs.calls().size(); CK_CHECK(paused.advance(stop.get_token()) == State::Paused && f.fs.calls().size() == prior && paused.abandon());
    CK_CHECK(paused.advance() == State::Abandoned && !paused.completed().value() && f.fs.live_cursors() == 0);
}
CK_TEST(destination_batch_empty_results_retain_parent_and_bounds_checked_getters_are_total) {
    Fixture f; DestinationBatchJob job(f.parent(), {}, NameEncoding::PosixBytes, {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(result->entries().empty() && result->pin_directory() && f.calls(Op::LookupChild) == 0);
    CK_CHECK(!result->probe(0) && !result->probe_failure(0) && !result->reported_object_leader(0) && result->exact_name_count(0) == 0);
}
CK_TEST(destination_batch_constructor_rejects_invalid_configuration_before_native_queries) {
    for (int mode = 0; mode < 9; ++mode) {
        Fixture f; DestinationBatchLimits limits; auto parent = mode == 0 ? std::shared_ptr<const DirectoryCursor>{} : f.parent();
        if (mode == 3) limits.rows = 0;
        if (mode == 4) limits.name_bytes = DestinationBatchLimits::hard_name_bytes + 1;
        if (mode == 5) limits.component_bytes = 0;
        if (mode == 6) limits.time_slice = MonotonicClock::Duration::zero();
        if (mode == 7) limits.time_slice = std::chrono::seconds(2);
        if (mode == 8) limits.rows = DestinationBatchLimits::hard_rows + 1;
        bool caught = false;
        try { DestinationBatchJob job(std::move(parent), {}, mode == 2 ? NameEncoding::WindowsEaBytes : NameEncoding::PosixBytes,
            limits, mode == 1 ? nullptr : f.fs.clock()); }
        catch (const std::invalid_argument&) { caught = true; }
        CK_CHECK(caught && f.calls(Op::LookupChild) == 0 && f.fs.live_cursors() == 0);
    }
}
CK_TEST(destination_batch_utf16_budget_and_win32_risk_and_reported_length_remain_independent) {
    for (const std::size_t budget : {std::size_t{29}, std::size_t{30}}) {
        Fixture f; const auto component = *NativeName::from_windows(u"CON"); LocationInfo location;
        location.family = LocationQueryFamily::WindowsHandle; location.directory = f.fs.metadata(f.root).identity;
        location.windows_component_utf16_max = Observation<std::uint64_t>::available(2);
        f.fs.set_location(f.root, Observation<LocationInfo>::available(location));
        DestinationBatchLimits limits; limits.name_bytes = budget;
        DestinationBatchJob job(f.parent(), {component}, NameEncoding::WindowsUtf16, limits, f.fs.clock());
        (void)job.advance();
        if (budget == 29) CK_CHECK(job.state() == State::Failed && f.calls(Op::LookupChild) == 0);
        else {
            const auto result = finish(job); const auto* probe = result->probe(0);
            CK_CHECK(take(result->entries()[0].mapped) == component && probe->win32_conventions()->has(Win32NameIssue::DeviceAliasRisk));
            CK_CHECK(take(probe->length()).exceeds_reported_maximum() && probe->lookup().value()->state() == ChildLookupState::Missing);
        }
    }
}
CK_TEST(destination_batch_clock_exception_before_mapping_releases_the_input_and_directory) {
    Fixture f; f.fs.clock()->throw_on_read = true;
    DestinationBatchJob job(f.parent(), {name("a")}, NameEncoding::PosixBytes, {}, f.fs.clock()); bool caught = false;
    try { (void)job.advance(); } catch (const std::runtime_error&) { caught = true; }
    CK_CHECK(caught && job.state() == State::Failed && job.failure()->error.operation.empty() && !job.completed().value());
    CK_CHECK(f.fs.live_cursors() == 0 && f.calls(Op::LookupChild) == 0);
}
