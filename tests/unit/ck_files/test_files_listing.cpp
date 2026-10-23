// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_listing.hpp"
#include "scratch_directory.hpp"
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <stdexcept>
#include <set>

namespace {
using namespace ck::files;
struct Script {
    std::shared_ptr<ck::testing::ManualMonotonicClock> clock = std::make_shared<ck::testing::ManualMonotonicClock>();
    MonotonicClock::Duration read_cost{}, open_cost{}, inspect_cost{};
    std::vector<NativeName> names;
    std::vector<ObservationState> opens, metadata;
    int reads = 0, child_opens = 0, inspections = 0, live = 0, maximum_live = 0, roots = 0, path_inspections = 0;
    int read_error = -1;
    bool skip_all = false, throw_read = false, throw_open = false, throw_inspect = false, null_child = false, null_root = false;
    bool metadata_only = true;
    enum class Cancel { None, Read, Open, Inspect } cancel = Cancel::None;
    std::stop_source stop;
    ObservationState root_state = ObservationState::Available;
    void cancel_at(Cancel stage) { if (cancel == stage) { stop.request_stop(); cancel = Cancel::None; } }
};
NativeError error(const char* operation) { return {ErrorDomain::Adapter, 99, operation}; }
class Entry final : public OpenedEntry {
public:
    Entry(std::shared_ptr<Script> script, std::size_t index) : script_(std::move(script)), index_(index) {
        ++script_->live; script_->maximum_live = std::max(script_->maximum_live, script_->live);
    }
    ~Entry() override { --script_->live; }
    Observation<BasicMetadata> inspect_basic() const override {
        ++script_->inspections; script_->clock->advance(script_->inspect_cost); script_->cancel_at(Script::Cancel::Inspect);
        if (script_->throw_inspect) throw std::runtime_error("injected inspection exception");
        const auto state = index_ < script_->metadata.size() ? script_->metadata[index_] : ObservationState::Available;
        if (state != ObservationState::Available) return Observation<BasicMetadata>::unavailable(state, error("fake.metadata"));
        BasicMetadata result; result.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        result.logical_size = Observation<std::uint64_t>::available(index_);
        return Observation<BasicMetadata>::available(std::move(result));
    }
private:
    std::shared_ptr<Script> script_; std::size_t index_;
};
class Cursor final : public DirectoryCursor {
public:
    explicit Cursor(std::shared_ptr<Script> script) : script_(std::move(script)) {}
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name, EntryAccess access) const override {
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        ++script_->child_opens; script_->clock->advance(script_->open_cost); script_->metadata_only = script_->metadata_only && access == EntryAccess::MetadataOnly;
        script_->cancel_at(Script::Cancel::Open);
        if (script_->throw_open) throw std::runtime_error("injected open exception");
        const auto found = std::find(script_->names.begin(), script_->names.end(), name);
        if (found == script_->names.end()) throw std::runtime_error("unexpected fake component");
        const auto index = static_cast<std::size_t>(found - script_->names.begin());
        const auto state = index < script_->opens.size() ? script_->opens[index] : ObservationState::Available;
        if (state != ObservationState::Available) return Result::unavailable(state, error("fake.open"));
        if (script_->null_child) return Result::available(nullptr);
        return Result::available(std::make_unique<Entry>(script_, index));
    }
private:
    Observation<DirectoryStep> next_step() override {
        using Result = Observation<DirectoryStep>;
        const int call = script_->reads++; script_->clock->advance(script_->read_cost);
        if (script_->throw_read) throw std::runtime_error("injected cursor exception");
        if (call == script_->read_error) return Result::unavailable(ObservationState::Denied, error("fake.enumeration"));
        if (script_->skip_all) return Result::available(DirectorySkip{});
        if (position_ == script_->names.size()) return Result::available(DirectoryEnd{});
        script_->cancel_at(Script::Cancel::Read); return Result::available(script_->names[position_++]);
    }
    std::shared_ptr<Script> script_; std::size_t position_ = 0;
};
class FileSystem final : public BasicFileSystem {
public:
    std::shared_ptr<Script> script = std::make_shared<Script>();
    Observation<std::unique_ptr<DirectoryCursor>> open_directory(const std::filesystem::path&, DirectoryLinkPolicy policy) const override {
        using Result = Observation<std::unique_ptr<DirectoryCursor>>;
        ++script->roots; if (policy != DirectoryLinkPolicy::RejectFinalLink) throw std::runtime_error("unexpected following policy");
        if (script->root_state != ObservationState::Available) return Result::unavailable(script->root_state, error("fake.root"));
        if (script->null_root) return Result::available(nullptr);
        return Result::available(std::make_unique<Cursor>(script));
    }
    Observation<BasicMetadata> inspect_basic(const std::filesystem::path&) const override {
        ++script->path_inspections; return Observation<BasicMetadata>::unavailable(ObservationState::Failed, error("unexpected.path"));
    }
};
Observation<std::unique_ptr<DirectoryListing>> open_listing(const FileSystem& filesystem,
    const std::filesystem::path& path, DirectoryListingLimits limits = {}) {
    return DirectoryListing::open(filesystem, path, limits, filesystem.script->clock);
}
std::shared_ptr<Script> script(std::initializer_list<const char*> names) {
    auto result = std::make_shared<Script>(); for (auto name : names) result->names.push_back(*NativeName::from_posix(name)); return result;
}
std::set<std::string> collect(DirectoryListing& listing) {
    std::set<std::string> result;
    for (int step = 0; step < 100; ++step) {
        auto batch = listing.read();
        for (const auto& entry : batch.entries) CK_CHECK(result.insert(*entry.name.posix_bytes()).second);
        if (batch.state == DirectoryReadState::End) return result;
        if (batch.state != DirectoryReadState::More) throw std::runtime_error("unexpected listing termination");
    }
    throw std::runtime_error("listing fixture budget exceeded");
}
}
CK_TEST(listing_streams_bounded_rows_and_releases_each_child_before_the_next) {
    auto source = script({"a", "b", "c"}); DirectoryListing listing(std::make_unique<Cursor>(source), {2, 8, 4}, source->clock);
    auto first = listing.read(); CK_CHECK(first.state == DirectoryReadState::More && first.entries.size() == 2);
    CK_CHECK(first.entries[0].metadata.value() && source->live == 0 && source->maximum_live == 1 && source->metadata_only);
    auto second = listing.read(); CK_CHECK(second.state == DirectoryReadState::End && second.entries.size() == 1);
    const int reads = source->reads; CK_CHECK(listing.read().state == DirectoryReadState::End && source->reads == reads);
}
CK_TEST(listing_byte_budget_retains_the_consumed_component_without_skips_or_duplicates) {
    auto source = script({"aa", "bbb", "c"}); DirectoryListing listing(std::make_unique<Cursor>(source), {10, 4, 4}, source->clock);
    auto first = listing.read(); CK_CHECK(first.entries.size() == 1 && source->reads == 2 && source->child_opens == 1);
    auto second = listing.read(); CK_CHECK(second.state == DirectoryReadState::End && second.entries.size() == 2);
    CK_CHECK(*second.entries[0].name.posix_bytes() == "bbb" && *second.entries[1].name.posix_bytes() == "c");
}
CK_TEST(listing_keeps_per_entry_open_and_metadata_errors_attached_to_native_names) {
    auto source = script({"a", "b", "c"}); source->opens = {ObservationState::Denied};
    source->metadata = {ObservationState::Available, ObservationState::Gone};
    DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock); auto batch = listing.read();
    CK_CHECK(batch.state == DirectoryReadState::End && batch.entries.size() == 3 && !batch.error);
    CK_CHECK(batch.entries[0].metadata.state() == ObservationState::Denied && batch.entries[0].metadata.unavailable_detail()->error == error("fake.open"));
    CK_CHECK(batch.entries[1].metadata.state() == ObservationState::Gone && batch.entries[1].metadata.unavailable_detail()->error == error("fake.metadata"));
    CK_CHECK(batch.entries[2].metadata.value() && source->live == 0);
}
CK_TEST(listing_cancellation_between_each_native_stage_preserves_resume_position_and_ownership) {
    for (auto stage : {Script::Cancel::Read, Script::Cancel::Open, Script::Cancel::Inspect}) {
        auto source = script({"a", "b"}); source->cancel = stage;
        DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock); auto paused = listing.read(source->stop.get_token());
        CK_CHECK(paused.state == DirectoryReadState::Cancelled && source->live == 0);
        auto seen = collect(listing); for (const auto& entry : paused.entries) CK_CHECK(seen.insert(*entry.name.posix_bytes()).second);
        CK_CHECK((seen == std::set<std::string>{"a", "b"}));
        if (stage == Script::Cancel::Read) CK_CHECK(source->child_opens == 2);
        if (stage == Script::Cancel::Open) CK_CHECK(source->child_opens == 3 && source->inspections == 2);
    }
    auto source = script({"a"}); DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock); source->stop.request_stop();
    CK_CHECK(listing.read(source->stop.get_token()).state == DirectoryReadState::Cancelled && source->reads == 0);
}
CK_TEST(listing_enumeration_error_keeps_prior_rows_and_is_terminal_instead_of_end) {
    auto source = script({"a", "b"}); source->read_error = 1; DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock);
    auto batch = listing.read(); CK_CHECK(batch.entries.size() == 1 && batch.state == DirectoryReadState::Error && batch.error);
    CK_CHECK(batch.error->error == error("fake.enumeration")); const int calls = source->reads;
    CK_CHECK(listing.read().state == DirectoryReadState::Error && source->reads == calls);
}
CK_TEST(listing_rejects_oversize_components_in_native_storage_bytes_before_opening_children) {
    for (bool windows : {false, true}) {
        auto source = script({}); source->names.push_back(windows ? *NativeName::from_windows(u"abc") : *NativeName::from_posix("abcde"));
        DirectoryListing listing(std::make_unique<Cursor>(source), {2, 4, 4}, source->clock); const auto batch = listing.read();
        CK_CHECK(batch.state == DirectoryReadState::Error && batch.entries.empty() && source->child_opens == 0);
        CK_CHECK(batch.error && batch.error->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
        CK_CHECK(listing.read().state == DirectoryReadState::Error && source->reads == 1);
    }
}
CK_TEST(listing_exceptions_poison_the_stream_and_close_temporary_handles_before_rethrow) {
    for (int stage = 0; stage < 3; ++stage) {
        auto source = script({"a"}); source->throw_read = stage == 0; source->throw_open = stage == 1; source->throw_inspect = stage == 2;
        DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock); bool caught = false;
        try { (void)listing.read(); } catch (const std::runtime_error&) { caught = true; }
        CK_CHECK(caught && source->live == 0); const int reads = source->reads, opens = source->child_opens;
        auto repeated = listing.read(); CK_CHECK(repeated.state == DirectoryReadState::Error && source->reads == reads && source->child_opens == opens);
        CK_CHECK(repeated.error->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead));
    }
}
CK_TEST(listing_empty_bounded_skip_batch_does_not_spin_until_an_entry_arrives) {
    auto source = script({}); source->skip_all = true; DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock);
    auto batch = listing.read(); CK_CHECK(batch.state == DirectoryReadState::More && batch.entries.empty());
    CK_CHECK(source->reads == static_cast<int>(DirectoryCursor::max_batch_size) && source->child_opens == 0);
}
CK_TEST(listing_factory_validates_limits_paths_and_null_results_without_unexpected_filesystem_calls) {
    FileSystem filesystem; filesystem.script->names = script({"a"})->names;
    for (const auto limits : {DirectoryListingLimits{0, 4, 4}, {4097, 4, 4}, {1, 0, 0}, {1, 4, 5}, {1, 9 * 1024 * 1024, 4}, {1, 2 * 1024 * 1024, 2 * 1024 * 1024}}) {
        bool rejected = false; try { (void)open_listing(filesystem, ck::testing::scratch_root(), limits); } catch (const std::invalid_argument&) { rejected = true; } CK_CHECK(rejected);
    }
    CK_CHECK(filesystem.script->roots == 0);
    CK_CHECK(open_listing(filesystem, "relative").state() == ObservationState::Failed && filesystem.script->roots == 0);
    filesystem.script->null_root = true; CK_CHECK(open_listing(filesystem, ck::testing::scratch_root()).state() == ObservationState::Failed);
    filesystem.script->null_root = false; auto opened = open_listing(filesystem, ck::testing::scratch_root());
    if (!opened.value()) throw std::runtime_error("fake listing open failed");
    CK_CHECK(collect(**opened.value()).size() == 1 && filesystem.script->path_inspections == 0);
}
CK_TEST(listing_factory_preserves_root_failures_and_null_children_are_explicit_row_failures) {
    FileSystem filesystem;
    for (auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        filesystem.script->root_state = state; const auto result = open_listing(filesystem, ck::testing::scratch_root());
        CK_CHECK(result.state() == state && result.unavailable_detail()->error == error("fake.root"));
    }
    auto source = script({"a"}); source->null_child = true; DirectoryListing listing(std::make_unique<Cursor>(source), {}, source->clock);
    auto batch = listing.read(); CK_CHECK(batch.state == DirectoryReadState::End && batch.entries.size() == 1 && batch.entries[0].metadata.state() == ObservationState::Failed);
}

CK_TEST(listing_time_slices_yield_after_each_slow_native_stage_without_reopening_prepared_children) {
    for (int stage = 0; stage < 3; ++stage) {
        auto source = script({"a", "b"});
        if (stage == 0) source->read_cost = MonotonicClock::Duration(5);
        if (stage == 1) source->open_cost = MonotonicClock::Duration(5);
        if (stage == 2) source->inspect_cost = MonotonicClock::Duration(5);
        DirectoryListing listing(std::make_unique<Cursor>(source), {8, 64, 64, MonotonicClock::Duration(5)}, source->clock);
        auto first = listing.read(); CK_CHECK(first.state == DirectoryReadState::More && first.time_slice_exhausted);
        if (stage == 0) CK_CHECK(first.entries.empty() && source->child_opens == 0);
        if (stage == 1) CK_CHECK(first.entries.empty() && source->child_opens == 1 && source->live == 1 && source->inspections == 0);
        if (stage == 2) CK_CHECK(first.entries.size() == 1 && source->live == 0);
        auto seen = collect(listing); for (const auto& entry : first.entries) CK_CHECK(seen.insert(*entry.name.posix_bytes()).second);
        CK_CHECK((seen == std::set<std::string>{"a", "b"}));
        CK_CHECK(source->child_opens == 2 && source->inspections == 2 && source->maximum_live == 1 && source->live == 0);
    }
}
CK_TEST(listing_cancellation_closes_a_prepared_handle_and_retains_its_pending_component) {
    auto source = script({"a"}); source->open_cost = MonotonicClock::Duration(5);
    DirectoryListing listing(std::make_unique<Cursor>(source), {8, 64, 64, MonotonicClock::Duration(5)}, source->clock);
    CK_CHECK(listing.read().time_slice_exhausted && source->live == 1);
    source->stop.request_stop(); const auto paused = listing.read(source->stop.get_token());
    CK_CHECK(paused.state == DirectoryReadState::Cancelled && !paused.time_slice_exhausted && source->live == 0);
    CK_CHECK(collect(listing).size() == 1 && source->child_opens == 2 && source->inspections == 1 && source->live == 0);
}
CK_TEST(listing_rejects_clock_reversal_across_slices_and_closes_prepared_handles) {
    auto source = script({"a"}); source->open_cost = MonotonicClock::Duration(5);
    DirectoryListing listing(std::make_unique<Cursor>(source), {8, 64, 64, MonotonicClock::Duration(5)}, source->clock);
    CK_CHECK(listing.read().time_slice_exhausted && source->live == 1);
    source->clock->value = MonotonicClock::TimePoint(MonotonicClock::Duration(4));
    const auto failed = listing.read(); CK_CHECK(failed.state == DirectoryReadState::Error && failed.error && source->live == 0);
    CK_CHECK(failed.error->error.operation == "clock.reversed"); const int reads = source->clock->reads;
    CK_CHECK(listing.read().state == DirectoryReadState::Error && source->clock->reads == reads);
}
CK_TEST(listing_clock_exception_poisoning_closes_a_prepared_native_object) {
    auto source = script({"a"}); source->open_cost = MonotonicClock::Duration(5);
    DirectoryListing listing(std::make_unique<Cursor>(source), {8, 64, 64, MonotonicClock::Duration(5)}, source->clock);
    CK_CHECK(listing.read().time_slice_exhausted && source->live == 1); source->clock->throw_on_read = true;
    bool caught = false; try { (void)listing.read(); } catch (const std::runtime_error&) { caught = true; }
    CK_CHECK(caught && source->live == 0); const int reads = source->clock->reads;
    CK_CHECK(listing.read().state == DirectoryReadState::Error && source->clock->reads == reads);
}
CK_TEST(listing_time_limit_and_clock_validation_precede_root_io) {
    FileSystem filesystem;
    for (auto duration : {MonotonicClock::Duration::zero(), MonotonicClock::Duration(-1), MonotonicClock::Duration::max()}) {
        DirectoryListingLimits limits; limits.time_slice = duration; bool rejected = false;
        try { (void)open_listing(filesystem, ck::testing::scratch_root(), limits); } catch (const std::invalid_argument&) { rejected = true; } CK_CHECK(rejected);
    }
    bool rejected = false;
    try { (void)DirectoryListing::open(filesystem, ck::testing::scratch_root(), {}, nullptr); } catch (const std::invalid_argument&) { rejected = true; }
    CK_CHECK(rejected && filesystem.script->roots == 0);
}
