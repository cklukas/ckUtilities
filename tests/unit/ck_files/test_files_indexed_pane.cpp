// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "indexed_pane.hpp"
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <functional>
namespace {
using namespace ck::files;
using Bytes = std::vector<std::byte>;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("sort fixture: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
struct File {
    Bytes data;
    bool closed = false;
    std::optional<UnavailableObservation> read_error, write_error;
    std::function<void()> on_read, on_write;
};
struct Control {
    std::vector<std::shared_ptr<File>> files;
    std::optional<UnavailableObservation> create_error, write_error;
    bool throw_create = false;
};
class ByteFile final : public IndexByteFile {
  public:
    explicit ByteFile(std::shared_ptr<File> file) : f(std::move(file)) {
    }
    ~ByteFile() override {
        f->closed = true;
        f->data.clear();
    }
    Observation<std::uint64_t> size() override {
        return Observation<std::uint64_t>::available(f->data.size());
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        if (f->write_error)
            return Observation<std::size_t>::unavailable(f->write_error->state, f->write_error->error);
        const auto at = static_cast<std::size_t>(offset);
        if (at + bytes.size() > f->data.size())
            f->data.resize(at + bytes.size());
        std::copy(bytes.begin(), bytes.end(), f->data.begin() + static_cast<std::ptrdiff_t>(at));
        if (f->on_write)
            f->on_write();
        return Observation<std::size_t>::available(bytes.size());
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) override {
        if (f->read_error)
            return Observation<std::size_t>::unavailable(f->read_error->state, f->read_error->error);
        const auto at = static_cast<std::size_t>(offset);
        const auto n = at < f->data.size() ? std::min(bytes.size(), f->data.size() - at) : 0;
        if (n)
            std::copy_n(f->data.begin() + static_cast<std::ptrdiff_t>(at), n, bytes.begin());
        if (f->on_read)
            f->on_read();
        return Observation<std::size_t>::available(n);
    }

  private:
    std::shared_ptr<File> f;
};
class Factory final : public IndexFileFactory {
  public:
    explicit Factory(std::shared_ptr<Control> value) : c(std::move(value)) {
    }
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        if (c->throw_create)
            throw std::runtime_error("create fault");
        if (c->create_error)
            return Observation<std::unique_ptr<IndexByteFile>>::unavailable(c->create_error->state,
                                                                            c->create_error->error);
        auto file = std::make_shared<File>();
        file->write_error = c->write_error;
        c->files.push_back(file);
        return Observation<std::unique_ptr<IndexByteFile>>::available(std::make_unique<ByteFile>(file));
    }

  private:
    std::shared_ptr<Control> c;
};

struct Script {
    AdapterScope scope;
    std::vector<NativeName> names;
    std::optional<UnavailableObservation> scope_error, read_error, metadata_error, root_error;
    std::function<void()> on_read, on_inspect;
    bool root_id = true, foreign_child = false, changed = false, foreign_root = false, scope_changed = false;
    bool throw_inspect = false;
    int reads = 0, inspections = 0, live = 0, opened = 0;
};
ObjectIdentity fixture_identity(const AdapterScope &scope, std::uint64_t id) {
    return {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(id), {}};
}
class Entry final : public OpenedEntry {
  public:
    explicit Entry(std::shared_ptr<Script> s) : s_(std::move(s)) {
    }
    Observation<BasicMetadata> inspect_basic() const override {
        if (s_->metadata_error)
            return Observation<BasicMetadata>::unavailable(s_->metadata_error->state, s_->metadata_error->error);
        BasicMetadata m;
        m.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        m.identity =
            Observation<ObjectIdentity>::available(fixture_identity(s_->foreign_child ? AdapterScope{} : s_->scope, 2));
        return Observation<BasicMetadata>::available(std::move(m));
    }

  private:
    std::shared_ptr<Script> s_;
};
class Cursor final : public DirectoryCursor {
  public:
    explicit Cursor(std::shared_ptr<Script> s)
        : DirectoryCursor(s->root_id ? Observation<ObjectIdentity>::available(fixture_identity(s->scope, 1))
                                     : Observation<ObjectIdentity>{}),
          s_(std::move(s)) {
        ++s_->live;
    }
    ~Cursor() override {
        --s_->live;
    }
    Observation<AdapterScope> source_scope() const override {
        if (s_->scope_error)
            return Observation<AdapterScope>::unavailable(s_->scope_error->state, s_->scope_error->error);
        return Observation<AdapterScope>::available(s_->scope_changed ? AdapterScope{} : s_->scope);
    }
    Observation<BasicMetadata> inspect_basic() const override {
        ++s_->inspections;
        if (s_->on_inspect)
            s_->on_inspect();
        if (s_->throw_inspect)
            throw std::runtime_error("root inspect");
        if (s_->root_error)
            return Observation<BasicMetadata>::unavailable(s_->root_error->state, s_->root_error->error);
        BasicMetadata m;
        m.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        if (s_->root_id)
            m.identity = Observation<ObjectIdentity>::available(
                fixture_identity(s_->foreign_root ? AdapterScope{} : s_->scope, 1));
        m.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(s_->changed ? 2 : 1, 0));
        return Observation<BasicMetadata>::available(std::move(m));
    }
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName &, EntryAccess access) const override {
        CK_CHECK(access == EntryAccess::MetadataOnly);
        ++s_->opened;
        return Observation<std::unique_ptr<OpenedEntry>>::available(std::make_unique<Entry>(s_));
    }

  private:
    Observation<DirectoryStep> next_step() override {
        ++s_->reads;
        if (s_->on_read)
            s_->on_read();
        if (s_->read_error)
            return Observation<DirectoryStep>::unavailable(s_->read_error->state, s_->read_error->error);
        if (at_ == s_->names.size())
            return Observation<DirectoryStep>::available(DirectoryEnd{});
        return Observation<DirectoryStep>::available(s_->names[at_++]);
    }
    std::shared_ptr<Script> s_;
    std::size_t at_ = 0;
};
struct Fixture {
    std::shared_ptr<Script> s = std::make_shared<Script>();
    std::shared_ptr<Control> c = std::make_shared<Control>();
    std::shared_ptr<IndexStorageBudget> budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 16);
    std::shared_ptr<IndexFileFactory> factory = std::make_shared<Factory>(c);
    std::shared_ptr<ck::testing::ManualMonotonicClock> clock = std::make_shared<ck::testing::ManualMonotonicClock>();
    PaneListingRequest request{AdapterScope{}, 42};
    std::array<std::byte, 16> generation{};
    Fixture(std::initializer_list<const char *> names = {"z", ".a", "b"}) {
        generation[0] = std::byte{7};
        for (auto name : names)
            s->names.push_back(*NativeName::from_posix(name));
    }
    std::unique_ptr<DirectoryIndexListingJob> job(StorageModel model = StorageModel::LinuxStatx,
                                                  DirectoryIndexStoreLimits limits = {}) {
        return std::make_unique<DirectoryIndexListingJob>(
            request,
            std::make_unique<DirectoryListing>(std::make_unique<Cursor>(s), DirectoryListingLimits{1, 4096, 4096},
                                               clock),
            model, factory, budget, generation, limits, DirectoryIndexSortLimits{{1, 4096}, 1});
    }
    void released() {
        CK_CHECK(s->live == 0 && budget->charged_files() == 0 && budget->charged_bytes() == 0);
    }
};
std::size_t finish(DirectoryIndexListingJob &job) {
    std::size_t steps = 0;
    while (job.state() == DirectoryIndexListingState::Collecting ||
           job.state() == DirectoryIndexListingState::SortingNames ||
           job.state() == DirectoryIndexListingState::CheckingNames) {
        if (++steps > 10000)
            throw std::runtime_error("listing step budget");
        job.advance();
    }
    return steps;
}

std::shared_ptr<const CompletedDirectoryIndex> collect(Fixture &f, PaneListingRequest request) {
    f.request = request;
    auto job = f.job();
    finish(*job);
    return take(job->completed());
}
void finish(IndexedPaneDataJob &job) {
    std::size_t steps = 0;
    while (job.state() == IndexedPaneDataJobState::Sorting || job.state() == IndexedPaneDataJobState::Indexing) {
        if (++steps > 10000)
            throw std::runtime_error("pane data step budget");
        job.advance();
    }
}
IndexedPane::Data prepare(std::shared_ptr<const CompletedDirectoryIndex> source, PaneOrder order) {
    IndexedPaneDataJob job(std::move(source), order, {{1, 4096}, 1}, {1, 4096});
    finish(job);
    return take(job.completed());
}
std::shared_ptr<const IndexedPanePage> page(IndexedPane &pane, std::uint64_t first = 0,
                                            IndexedPanePageLimits limits = {}) {
    auto result = pane.capture_page(pane.state_stamp(), first, limits);
    CK_CHECK(result.state == IndexedPaneCaptureState::Ready && result.page);
    if (!result.page)
        throw std::runtime_error("missing indexed pane page");
    return result.page;
}
constexpr PaneOrder ascending{PaneSortKey::Name, false, false}, descending{PaneSortKey::Name, true, false};
} // namespace
CK_TEST(indexed_pane_publishes_bound_ordered_pages_without_native_owners) {
    Fixture f;
    std::shared_ptr<const IndexedPanePage> saved;
    {
        IndexedPane pane;
        auto request = pane.begin_listing(ascending);
        CK_CHECK(pane.listing_pending());
        CK_CHECK(pane.publish(prepare(collect(f, request), ascending)) == IndexedPanePublish::Applied);
        saved = page(pane, 0, {{2, 4096}, 4096});
        CK_CHECK(saved->rows().size() == 2 && saved->total_rows() == 3 && saved->next() == 2);
        CK_CHECK(saved->rows()[0].display_name == ".a" && saved->rows()[0].id.number == 2 && saved->rows()[0].focused);
        CK_CHECK(saved->rows()[1].display_name == "b" && !saved->rows()[1].focused && saved->stamp().current());
        CK_CHECK(saved->rows()[0].metadata.identity.value()->scope == f.s->scope);
        CK_CHECK(saved->rows()[0].id.scope != f.s->scope && !saved->rows()[0].marked);
        CK_CHECK(page(pane, 2)->rows()[0].display_name == "z" && page(pane, 3)->end());
    }
    f.released();
    CK_CHECK(!saved->stamp().current() && saved->rows()[0].display_name == ".a");
}
CK_TEST(indexed_pane_rejects_stale_foreign_and_wrong_order_results_before_application) {
    Fixture f;
    IndexedPane pane, other;
    auto old = pane.begin_listing(ascending);
    auto stale = prepare(collect(f, old), ascending);
    auto current = pane.begin_listing(descending);
    CK_CHECK(pane.publish(stale) == IndexedPanePublish::Stale && pane.listing_pending());
    CK_CHECK(other.publish(stale) == IndexedPanePublish::Stale);
    auto wrong = prepare(collect(f, current), ascending);
    CK_CHECK(pane.publish(wrong) == IndexedPanePublish::InvalidData && pane.rows() == 0);
    CK_CHECK(pane.publish(prepare(collect(f, current), descending)) == IndexedPanePublish::Applied);
    CK_CHECK(page(pane)->rows()[0].display_name == "z");
}
CK_TEST(indexed_pane_failed_and_cancelled_navigation_preserve_previous_dataset_and_revoke_stamps) {
    Fixture f;
    IndexedPane pane;
    auto req = pane.begin_listing(ascending);
    pane.publish(prepare(collect(f, req), ascending));
    auto before = page(pane);
    auto pending = pane.begin_listing(descending);
    CK_CHECK(!before->stamp().current());
    auto during = page(pane);
    CK_CHECK(during->listing_pending() && during->rows()[0].display_name == ".a");
    CK_CHECK(pane.focus(during->stamp(), during->rows()[0].id) == IndexedPaneFocus::Unavailable);
    CK_CHECK(!pane.fail_listing(req, {ObservationState::Denied, {ErrorDomain::Posix, 13, "old"}}));
    CK_CHECK(pane.fail_listing(pending, {ObservationState::Denied, {ErrorDomain::Posix, 13, "navigation"}}));
    CK_CHECK(!during->stamp().current());
    auto failed = page(pane);
    CK_CHECK(failed->listing_error()->error.operation == "navigation" && !failed->listing_pending());
    auto cancelled = pane.begin_listing(ascending);
    CK_CHECK(pane.cancel_listing(cancelled) && !pane.cancel_listing(cancelled));
    CK_CHECK(page(pane)->rows()[0].id == failed->rows()[0].id && !pane.listing_error());
}
CK_TEST(indexed_pane_reordering_retains_original_focus_and_dataset_namespace) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto before = page(pane);
    auto focused = before->rows()[1].id;
    CK_CHECK(pane.focus(before->stamp(), focused) == IndexedPaneFocus::Applied && !before->stamp().current());
    auto order = take(pane.begin_reorder(pane.state_stamp(), descending));
    IndexedPaneDataJob job(pane.data(), order, descending, {{1, 4096}, 1}, {1, 4096});
    finish(job);
    CK_CHECK(pane.publish(take(job.completed())) == IndexedPanePublish::Applied);
    auto sorted = page(pane);
    CK_CHECK(sorted->rows()[0].display_name == "z" && sorted->rows()[1].id == focused && sorted->rows()[1].focused);
    CK_CHECK(sorted->rows()[2].id == before->rows()[0].id && pane.focused() == focused);
    CK_CHECK(pane.focus(before->stamp(), focused) == IndexedPaneFocus::Stale);
}
CK_TEST(indexed_pane_rapid_reorders_and_foreign_dataset_replies_cannot_replace_current_state) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto olddata = pane.data();
    auto first = take(pane.begin_reorder(pane.state_stamp(), descending));
    IndexedPaneDataJob one(olddata, first, descending);
    finish(one);
    auto next = take(pane.begin_reorder(pane.state_stamp(), ascending));
    IndexedPaneDataJob two(olddata, next, ascending);
    finish(two);
    CK_CHECK(pane.publish(take(one.completed())) == IndexedPanePublish::Stale && pane.listing_pending());
    CK_CHECK(pane.publish(take(two.completed())) == IndexedPanePublish::Applied);
    auto same_dataset = take(pane.begin_reorder(pane.state_stamp(), descending));
    IndexedPaneDataJob old_revision(olddata, same_dataset, descending);
    finish(old_revision);
    CK_CHECK(pane.publish(take(old_revision.completed())) == IndexedPanePublish::InvalidData);
    CK_CHECK(pane.cancel_listing(same_dataset));
    auto nav = pane.begin_listing(ascending);
    pane.publish(prepare(collect(f, nav), ascending));
    auto reorder = take(pane.begin_reorder(pane.state_stamp(), descending));
    IndexedPaneDataJob wrong(olddata, reorder, descending);
    finish(wrong);
    CK_CHECK(pane.publish(take(wrong.completed())) == IndexedPanePublish::InvalidData && pane.listing_pending());
}
CK_TEST(indexed_pane_display_pages_keep_escaping_metadata_errors_and_exact_text_budgets) {
    Fixture f({"a"});
    f.s->names[0] = *NativeName::from_posix(std::string("x\xFF", 2));
    f.s->metadata_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "denied"}};
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto full = page(pane);
    CK_CHECK(full->rows()[0].display_name == "x\\xFF" &&
             full->rows()[0].inspection_error->state == ObservationState::Denied);
    const auto used = full->text_bytes();
    CK_CHECK(used == 11);
    const auto stamp = pane.state_stamp();
    CK_CHECK(pane.capture_page(stamp, 0, {{1, used - 1}, 4096}).state == IndexedPaneCaptureState::LimitExceeded);
    CK_CHECK(pane.accepts(stamp) && page(pane, 0, {{1, used}, 4096})->text_bytes() == used);
    CK_CHECK(pane.capture_page(stamp, 0, {{1, 4096}, 16}).state == IndexedPaneCaptureState::LimitExceeded &&
             pane.accepts(stamp));
}
CK_TEST(indexed_pane_cancelled_page_and_focus_requests_leave_current_dataset_healthy) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto before = page(pane);
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(pane.capture_page(before->stamp(), 0, {}, stop.get_token()).state == IndexedPaneCaptureState::Cancelled);
    CK_CHECK(pane.focus(before->stamp(), before->rows()[1].id, stop.get_token()) == IndexedPaneFocus::Cancelled &&
             pane.accepts(before->stamp()));
    std::stop_source during;
    for (auto &file : f.c->files)
        if (!file->closed)
            file->on_read = [&] { during.request_stop(); };
    CK_CHECK(pane.capture_page(before->stamp(), 0, {}, during.get_token()).state == IndexedPaneCaptureState::Cancelled);
    for (auto &file : f.c->files)
        file->on_read = {};
    CK_CHECK(pane.accepts(before->stamp()) && page(pane)->rows().size() == 3);
}
CK_TEST(indexed_pane_data_jobs_detect_root_changes_and_preserve_unknown_continuity_evidence) {
    Fixture f;
    IndexedPane pane;
    auto request = pane.begin_listing(ascending);
    auto listing = collect(f, request);
    f.s->changed = true;
    IndexedPaneDataJob changed(listing, ascending);
    finish(changed);
    CK_CHECK(changed.state() == IndexedPaneDataJobState::Failed &&
             changed.failure()->state == ObservationState::Changed);
    Fixture g;
    g.s->root_id = false;
    g.s->root_error = UnavailableObservation{ObservationState::Unknown, {}};
    auto collected = collect(g, pane.begin_listing(ascending));
    g.s->root_error.reset();
    auto data = prepare(collected, ascending);
    // A newly available time must survive a later denied query, then reject a change.
    g.s->root_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "root"}};
    PaneListingRequest target{data->request().scope, data->request().number + 1};
    IndexedPaneDataJob uncertain(data, target, descending);
    finish(uncertain);
    auto unknown = take(uncertain.completed());
    CK_CHECK(unknown->after().state() == ObservationState::Denied);
    g.s->root_error.reset();
    g.s->changed = true;
    target.number++;
    IndexedPaneDataJob later(unknown, target, ascending);
    finish(later);
    CK_CHECK(later.state() == IndexedPaneDataJobState::Failed && later.failure()->state == ObservationState::Changed);
}
CK_TEST(indexed_pane_data_job_cancellation_at_each_advance_preserves_collection) {
    Fixture f;
    IndexedPane pane;
    auto listing = collect(f, pane.begin_listing(descending));
    IndexedPaneDataJob baseline(listing, descending, {{1, 4096}, 1}, {1, 4096});
    std::size_t steps = 0;
    while (baseline.state() != IndexedPaneDataJobState::Complete) {
        baseline.advance();
        if (++steps > 1000)
            throw std::runtime_error("baseline");
    }
    baseline.abandon();
    const auto bytes = f.budget->charged_bytes();
    const auto files = f.budget->charged_files();
    for (std::size_t at = 0; at < steps; ++at) {
        IndexedPaneDataJob job(listing, descending, {{1, 4096}, 1}, {1, 4096});
        for (std::size_t i = 0; i < at; ++i)
            job.advance();
        std::stop_source stop;
        stop.request_stop();
        job.advance(stop.get_token());
        CK_CHECK(job.state() == IndexedPaneDataJobState::Cancelled && !job.completed().value());
        CK_CHECK(listing->store()->state() == DirectoryIndexStoreState::Sealed && f.budget->charged_bytes() == bytes &&
                 f.budget->charged_files() == files);
    }
}
CK_TEST(indexed_pane_capture_io_failures_and_exceptions_revoke_pages_without_prefix) {
    for (bool throwing : {false, true}) {
        Fixture f;
        IndexedPane pane;
        pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
        auto before = page(pane);
        for (auto &file : f.c->files)
            if (!file->closed) {
                if (throwing)
                    file->on_read = [] { throw std::runtime_error("read fault"); };
                else
                    file->read_error =
                        UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "read"}};
            }
        bool threw = false;
        try {
            auto result = pane.capture_page(before->stamp(), 0);
            CK_CHECK(result.state == IndexedPaneCaptureState::Failed && !result.page);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw == throwing && !before->stamp().current());
    }
    Fixture source_failure;
    IndexedPane target;
    auto listing = collect(source_failure, target.begin_listing(ascending));
    auto data = prepare(listing, ascending);
    const UnavailableObservation denied{ObservationState::Denied, {ErrorDomain::Posix, 13, "source_failed"}};
    for (auto &file : source_failure.c->files)
        if (!file->closed)
            file->read_error = denied;
    CK_CHECK(!listing->store()->read_page(take(listing->store()->begin_scan())).value());
    CK_CHECK(target.publish(data) == IndexedPanePublish::Failed && target.listing_error()->error == denied.error);
}
CK_TEST(indexed_pane_invalid_and_stale_requests_leave_stamps_unchanged_without_IO) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto current = page(pane);
    PaneModel foreign;
    CK_CHECK(pane.capture_page(foreign.state_stamp(), 0).state == IndexedPaneCaptureState::Stale);
    CK_CHECK(pane.capture_page(current->stamp(), 4).state == IndexedPaneCaptureState::OutOfRange);
    CK_CHECK(pane.focus(current->stamp(), {AdapterScope{}, 1}) == IndexedPaneFocus::InvalidTarget);
    bool threw = false;
    try {
        pane.begin_listing({static_cast<PaneSortKey>(-1), false, false});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && pane.accepts(current->stamp()));
    auto pending = pane.begin_listing(ascending);
    auto stamp = pane.state_stamp();
    CK_CHECK(!pane.begin_reorder(stamp, descending).value() && pane.accepts(stamp));
    threw = false;
    try {
        pane.fail_listing(pending, {ObservationState::Available, {}});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && pane.listing_pending() && pane.accepts(stamp));
}
CK_TEST(indexed_pane_empty_state_and_empty_dataset_have_valid_owner_free_end_pages) {
    IndexedPane pane;
    auto empty = page(pane);
    CK_CHECK(empty->end() && empty->rows().empty() && !empty->listing_pending());
    Fixture f({});
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto ready = page(pane);
    CK_CHECK(ready->end() && ready->total_rows() == 0 && !pane.focused() && !empty->stamp().current());
}

CK_TEST(indexed_pane_preparation_limits_failures_and_abandonment_keep_collection_usable) {
    Fixture f;
    IndexedPane pane;
    auto listing = collect(f, pane.begin_listing(ascending));
    const auto bytes = f.budget->charged_bytes();
    const auto files = f.budget->charged_files();
    IndexedPaneDataJob limited(listing, ascending, {}, {}, 1);
    finish(limited);
    CK_CHECK(limited.state() == IndexedPaneDataJobState::Failed && !limited.completed().value());
    CK_CHECK(f.budget->charged_bytes() == bytes && f.budget->charged_files() == files);
    bool threw = false;
    try {
        IndexedPaneDataJob invalid(listing, {static_cast<PaneSortKey>(-1), false, false});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && listing->store()->state() == DirectoryIndexStoreState::Sealed);
    IndexedPaneDataJob abandoned(listing, descending);
    CK_CHECK(abandoned.abandon() && !abandoned.abandon() && !abandoned.completed().value());
    CK_CHECK(f.budget->charged_bytes() == bytes && f.budget->charged_files() == files);
}
CK_TEST(indexed_pane_request_changes_during_IO_discard_page_focus_and_publication) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto before = page(pane);
    bool triggered = false;
    PaneListingRequest newer;
    const auto arm = [&] {
        triggered = false;
        for (auto &file : f.c->files)
            if (!file->closed)
                file->on_read = [&] {
                    if (!triggered) {
                        triggered = true;
                        newer = pane.begin_listing(descending);
                    }
                };
    };
    arm();
    auto captured = pane.capture_page(before->stamp(), 0);
    CK_CHECK(captured.state == IndexedPaneCaptureState::Stale && !captured.page && pane.listing_pending());
    pane.cancel_listing(newer);
    auto stamp = pane.state_stamp();
    arm();
    CK_CHECK(pane.focus(stamp, before->rows()[1].id) == IndexedPaneFocus::Stale &&
             pane.focused() == before->rows()[0].id);
    for (auto &file : f.c->files)
        file->on_read = {};
    Fixture g;
    IndexedPane target;
    auto request = target.begin_listing(ascending);
    auto data = prepare(collect(g, request), ascending);
    triggered = false;
    for (auto &file : g.c->files)
        if (!file->closed)
            file->on_read = [&] {
                if (!triggered) {
                    triggered = true;
                    target.begin_listing(descending);
                }
            };
    CK_CHECK(target.publish(data) == IndexedPanePublish::Stale && target.rows() == 0 && target.listing_pending());
    Fixture h;
    IndexedPane newer_target;
    newer_target.publish(prepare(collect(h, newer_target.begin_listing(ascending)), ascending));
    auto old_stamp = newer_target.state_stamp();
    std::shared_ptr<File> source_file;
    for (auto &file : h.c->files)
        if (!file->closed) {
            source_file = file;
            break;
        }
    bool changed = false;
    PaneStateStamp new_stamp;
    for (auto &file : h.c->files)
        if (!file->closed)
            file->on_read = [&] {
                if (!changed) {
                    changed = true;
                    newer_target.begin_listing(descending);
                    new_stamp = newer_target.state_stamp();
                    source_file->data[12] ^= std::byte{1};
                }
            };
    auto stale_error = newer_target.capture_page(old_stamp, 0);
    CK_CHECK(stale_error.state == IndexedPaneCaptureState::Stale && !stale_error.page);
    CK_CHECK(newer_target.accepts(new_stamp) && !newer_target.listing_error() && newer_target.listing_pending());
}
