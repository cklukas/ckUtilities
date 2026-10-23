// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_listing_job.hpp"
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <stdexcept>
#include <type_traits>
namespace {
using namespace ck::files;
static_assert(!std::is_default_constructible_v<CompletedPaneListing>);
struct Script {
    std::vector<NativeName> names;
    std::vector<BasicMetadata> metadata;
    int reads = 0, roots = 0, opens = 0, live = 0, fail_read = -1, deny_open = -1;
    std::shared_ptr<ck::testing::ManualMonotonicClock> clock = std::make_shared<ck::testing::ManualMonotonicClock>();
    MonotonicClock::Duration open_cost{};
    bool throw_open = false, cancel_inspection = false;
    std::stop_source stop;
    AdapterScope scope;
};
NativeError error(const char* operation) { return {ErrorDomain::Adapter, 99, operation}; }
ObjectIdentity test_identity(const Script& script) { return {script.scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}}; }
class Entry final : public OpenedEntry {
public:
    Entry(std::shared_ptr<Script> source, std::size_t index) : source_(std::move(source)), index_(index) { ++source_->live; }
    ~Entry() override { --source_->live; }
    Observation<BasicMetadata> inspect_basic() const override {
        if (source_->cancel_inspection) { source_->stop.request_stop(); source_->cancel_inspection = false; }
        if (index_ < source_->metadata.size()) return Observation<BasicMetadata>::available(source_->metadata[index_]);
        BasicMetadata result; result.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile); return Observation<BasicMetadata>::available(std::move(result));
    }
private: std::shared_ptr<Script> source_; std::size_t index_;
};
class Cursor final : public DirectoryCursor {
public:
    explicit Cursor(std::shared_ptr<Script> source) : DirectoryCursor(Observation<ObjectIdentity>::available(test_identity(*source))), source_(std::move(source)) { ++source_->roots; }
    ~Cursor() override { --source_->roots; }
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name, EntryAccess access) const override {
        if (access != EntryAccess::MetadataOnly) throw std::runtime_error("unexpected access");
        ++source_->opens; source_->clock->advance(source_->open_cost); if (source_->throw_open) throw std::runtime_error("injected open exception");
        const auto found = std::find(source_->names.begin(), source_->names.end(), name);
        if (found == source_->names.end()) throw std::runtime_error("unexpected child");
        const auto index = static_cast<std::size_t>(found - source_->names.begin());
        if (static_cast<int>(index) == source_->deny_open) return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Denied, error("fake.open"));
        return Observation<std::unique_ptr<OpenedEntry>>::available(std::make_unique<Entry>(source_, index));
    }
private:
    Observation<DirectoryStep> next_step() override {
        const int step = source_->reads++;
        if (step == source_->fail_read) return Observation<DirectoryStep>::unavailable(ObservationState::Denied, error("fake.read"));
        if (at_ == source_->names.size()) return Observation<DirectoryStep>::available(DirectoryEnd{});
        return Observation<DirectoryStep>::available(source_->names[at_++]);
    }
    std::shared_ptr<Script> source_; std::size_t at_ = 0;
};
std::shared_ptr<Script> script(std::initializer_list<const char*> names) {
    auto result = std::make_shared<Script>(); for (auto name : names) result->names.push_back(*NativeName::from_posix(name)); return result;
}
std::unique_ptr<DirectoryListing> listing(std::shared_ptr<Script> source, std::size_t rows = 1) {
    auto clock = source->clock;
    return std::make_unique<DirectoryListing>(std::make_unique<Cursor>(std::move(source)), DirectoryListingLimits{rows, 128, 128}, std::move(clock));
}
PaneListingJob::Result finish(PaneListingJob& job) {
    for (int steps = 0; steps < 100; ++steps) {
        if (job.advance() == PaneListingJobState::Complete) { auto result = job.completed(); if (result.value()) return *result.value(); }
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("pane job failed");
    }
    throw std::runtime_error("pane job fixture exceeded budget");
}
void old_rows(PaneModel& pane) {
    std::vector<PaneEntry> entries; entries.emplace_back(*NativeName::from_posix("old"), BasicMetadata{}, Observation<bool>::available(false));
    if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) != PanePublish::Applied) throw std::runtime_error("cannot initialize pane");
}
}
CK_TEST(pane_job_stages_until_end_and_publishes_directory_and_rows_together) {
    PaneModel pane; old_rows(pane); const auto old = pane.focused(); auto source = script({"b", "a"});
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), StorageModel::LinuxStatx);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && !job.completed().value());
    CK_CHECK(pane.focused() == old && pane.visible_row(0)->display_name == "old" && source->roots == 1);
    const auto result = finish(job); CK_CHECK(source->roots == 0 && pane.publish(*result) == PanePublish::Applied);
    CK_CHECK(pane.size() == 2 && pane.visible_row(0)->display_name == "a");
    if (!pane.listing_directory().value()) throw std::runtime_error("published directory missing");
    CK_CHECK(compare_identity(*pane.listing_directory().value(), test_identity(*source)) == IdentityComparison::Same);
    const int reads = source->reads; CK_CHECK(job.advance() == PaneListingJobState::Complete && source->reads == reads);
    CK_CHECK(pane.publish(*result) == PanePublish::Stale);
}
CK_TEST(pane_job_completed_results_keep_scope_and_generation_and_reject_stale_publication) {
    PaneModel pane, other; old_rows(pane);
    PaneListingJob stale(pane.begin_listing(PaneListingPurpose::Navigate), listing(script({"stale"})), StorageModel::LinuxStatx);
    const auto old_result = finish(stale); const auto current = pane.begin_listing(PaneListingPurpose::Navigate);
    CK_CHECK(pane.publish(*old_result) == PanePublish::Stale && pane.listing_pending() && pane.visible_row(0)->display_name == "old");
    PaneListingJob foreign(other.begin_listing(PaneListingPurpose::Navigate), listing(script({"foreign"})), StorageModel::LinuxStatx);
    CK_CHECK(pane.publish(*finish(foreign)) == PanePublish::Stale && pane.listing_pending());
    PaneListingJob fresh(current, listing(script({"new"})), StorageModel::LinuxStatx);
    CK_CHECK(pane.publish(*finish(fresh)) == PanePublish::Applied && pane.visible_row(0)->display_name == "new");
}
CK_TEST(pane_job_keeps_entry_inspection_errors_in_rows_and_source_candidates) {
    PaneModel pane; auto source = script({"denied", "readable"}); source->deny_open = 0;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), StorageModel::LinuxStatx);
    CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.visible_size() == 2);
    const auto* row = pane.visible_row(0); if (!row || !row->entry.inspection_error()) throw std::runtime_error("inspection error missing");
    CK_CHECK(row->entry.inspection_error()->error == error("fake.open") && !row->entry.metadata.kind.value());
    CK_CHECK(pane.focus(row->id)); const auto candidates = pane.sources();
    CK_CHECK(candidates.candidates.size() == 1 && candidates.candidates[0].inspection_error && candidates.candidates[0].inspection_error->error == error("fake.open"));
}
CK_TEST(pane_job_enumeration_failure_discards_staging_and_preserves_published_selection) {
    PaneModel pane; old_rows(pane); const auto old = pane.focused(); pane.mark_visible(true); auto source = script({"a", "b"}); source->fail_read = 1;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Refresh), listing(source), StorageModel::LinuxStatx);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && job.advance() == PaneListingJobState::Failed && source->roots == 0);
    if (!job.failure()) throw std::runtime_error("missing job failure");
    CK_CHECK(job.failure()->error == error("fake.read") && !job.completed().value());
    CK_CHECK(pane.fail_listing(job.request(), *job.failure()) && pane.focused() == old && pane.marked(*old));
    const int reads = source->reads; CK_CHECK(job.advance() == PaneListingJobState::Failed && source->reads == reads);
}
CK_TEST(pane_job_cancelled_batches_are_retained_but_cannot_be_published_before_resumption) {
    PaneModel pane; old_rows(pane); auto source = script({"a", "b"}); source->cancel_inspection = true;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source, 4), StorageModel::LinuxStatx);
    CK_CHECK(job.advance(source->stop.get_token()) == PaneListingJobState::Paused && !job.completed().value());
    CK_CHECK(pane.visible_row(0)->display_name == "old" && pane.listing_pending());
    CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.size() == 2);
}
CK_TEST(pane_job_resource_limits_duplicates_and_encoding_mismatch_fail_without_partial_publication) {
    for (int mode = 0; mode < 5; ++mode) {
        PaneModel pane; old_rows(pane); auto source = script({"aa", "bb"}); PaneLimits limits;
        if (mode == 0) limits.rows = 1;
        if (mode == 1) limits.native_name_bytes = 1;
        if (mode == 2) limits.display_name_bytes = 1;
        if (mode == 3) source->names[1] = source->names[0];
        if (mode == 4) source->names[1] = *NativeName::from_windows(u"b");
        PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source, 4), StorageModel::Unknown, limits);
        CK_CHECK(job.advance() == PaneListingJobState::Failed && !job.completed().value() && source->roots == 0);
        CK_CHECK(pane.size() == 1 && pane.visible_row(0)->display_name == "old");
    }
}
CK_TEST(pane_job_exceptions_poison_staging_release_cursor_and_rethrow) {
    PaneModel pane; old_rows(pane); auto source = script({"a"}); source->throw_open = true;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), StorageModel::LinuxStatx);
    bool caught = false; try { (void)job.advance(); } catch (const std::runtime_error&) { caught = true; }
    CK_CHECK(caught && job.state() == PaneListingJobState::Failed && source->roots == 0);
    CK_CHECK(job.advance() == PaneListingJobState::Failed && pane.visible_row(0)->display_name == "old");
}
CK_TEST(pane_job_hidden_policy_uses_explicit_native_model_and_keeps_unknown_entries_visible) {
    for (auto model : {StorageModel::LinuxStatx, StorageModel::DarwinStatFlags, StorageModel::WindowsHandleAttributes}) {
        PaneModel pane; auto source = script({".dot", "flag", "plain", "unknown"});
        if (model == StorageModel::WindowsHandleAttributes) {
            source->names.clear(); for (auto text : {u".dot", u"flag", u"plain", u"unknown"}) source->names.push_back(*NativeName::from_windows(text));
        }
        source->metadata.resize(4);
        for (auto& metadata : source->metadata) metadata.bsd_flags = metadata.windows_attributes = Observation<std::uint32_t>::available(0);
        if (model == StorageModel::DarwinStatFlags) source->metadata[1].bsd_flags = Observation<std::uint32_t>::available(0x8000);
        if (model == StorageModel::WindowsHandleAttributes) source->metadata[1].windows_attributes = Observation<std::uint32_t>::available(2);
        source->metadata[3].bsd_flags = source->metadata[3].windows_attributes = Observation<std::uint32_t>::unavailable(ObservationState::Denied, error("fake.hidden"));
        PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), model);
        CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.size() == 4);
        CK_CHECK(pane.visible_size() == (model == StorageModel::DarwinStatFlags ? 2 : 3));
        auto view = pane.view(); view.show_hidden = true; pane.set_view(view); CK_CHECK(pane.visible_size() == 4);
    }
}
CK_TEST(pane_job_validates_ownership_limits_and_model_before_reading) {
    PaneModel pane; const auto request = pane.begin_listing(PaneListingPurpose::Navigate);
    for (int mode = 0; mode < 4; ++mode) {
        auto source = script({"a"}); PaneLimits limits; if (mode == 0) limits.rows = 0;
        auto token = request; if (mode == 1) token.number = 0;
        auto cursor = mode == 2 ? std::unique_ptr<DirectoryListing>{} : listing(source);
        bool rejected = false;
        try { PaneListingJob job(token, std::move(cursor), mode == 3 ? static_cast<StorageModel>(999) : StorageModel::LinuxStatx, limits); }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected && source->reads == 0 && source->roots == 0);
    }
}

CK_TEST(pane_job_abandonment_closes_a_prepared_child_and_cursor_and_never_resumes_io) {
    PaneModel pane; old_rows(pane); auto source = script({"a", "b"}); source->open_cost = std::chrono::milliseconds(20);
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), StorageModel::LinuxStatx);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && source->roots == 1 && source->live == 1);
    const int reads = source->reads, opens = source->opens;
    CK_CHECK(job.abandon() && job.state() == PaneListingJobState::Abandoned && source->roots == 0 && source->live == 0);
    CK_CHECK(!job.abandon() && !job.failure() && !job.completed().value());
    CK_CHECK(job.advance() == PaneListingJobState::Abandoned && source->reads == reads && source->opens == opens);
    CK_CHECK(pane.cancel_listing(job.request()) && pane.visible_row(0)->display_name == "old" && !pane.listing_pending());
}
CK_TEST(pane_generation_cancellation_rejects_an_already_issued_completion_after_job_abandonment) {
    PaneModel pane; old_rows(pane); auto source = script({"late"});
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), StorageModel::LinuxStatx);
    const auto issued = finish(job); CK_CHECK(job.abandon() && !job.completed().value());
    CK_CHECK(pane.cancel_listing(job.request()) && pane.publish(*issued) == PanePublish::Stale);
    CK_CHECK(pane.visible_row(0)->display_name == "old" && job.advance() == PaneListingJobState::Abandoned);
    PaneListingJob fresh(pane.begin_listing(PaneListingPurpose::Navigate), listing(script({"new"})), StorageModel::LinuxStatx);
    CK_CHECK(pane.publish(*finish(fresh)) == PanePublish::Applied && pane.publish(*issued) == PanePublish::Stale);
}
CK_TEST(pane_job_abandonment_preserves_existing_failures_for_review) {
    PaneModel pane; auto source = script({"a"}); source->fail_read = 0;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(source), StorageModel::LinuxStatx);
    CK_CHECK(job.advance() == PaneListingJobState::Failed && !job.abandon() && job.state() == PaneListingJobState::Failed);
    if (!job.failure()) throw std::runtime_error("failed job diagnostic lost");
    CK_CHECK(job.failure()->error == error("fake.read") && source->roots == 0 && pane.fail_listing(job.request(), *job.failure()));
}
