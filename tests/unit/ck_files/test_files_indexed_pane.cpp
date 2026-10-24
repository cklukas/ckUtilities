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
    std::optional<std::size_t> read_max, write_max;
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
        if (f->write_error) {
            if (f->on_write)
                f->on_write();
            return Observation<std::size_t>::unavailable(f->write_error->state, f->write_error->error);
        }
        if (f->write_max)
            bytes = bytes.first(std::min(bytes.size(), *f->write_max));
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
        auto n = at < f->data.size() ? std::min(bytes.size(), f->data.size() - at) : 0;
        if (f->read_max)
            n = std::min(n, *f->read_max);
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
    bool throw_child_inspect = false, missing_child_id = false, missing_child_kind = false,
         unknown_child_scheme = false;
    std::uint64_t child_id = 2;
    ObjectKind child_kind = ObjectKind::RegularFile;
    std::string child_extra_error;
    std::optional<UnavailableObservation> child_identity_error, child_kind_error;
    std::function<void()> on_child_inspect;
    int child_live = 0;
    int reads = 0, inspections = 0, live = 0, opened = 0;
};
ObjectIdentity fixture_identity(const AdapterScope &scope, std::uint64_t id) {
    return {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(id), {}};
}
class Entry final : public OpenedEntry {
  public:
    explicit Entry(std::shared_ptr<Script> s) : s_(std::move(s)) {
        ++s_->child_live;
    }
    ~Entry() override {
        --s_->child_live;
    }
    Observation<BasicMetadata> inspect_basic() const override {
        if (s_->on_child_inspect)
            s_->on_child_inspect();
        if (s_->throw_child_inspect)
            throw std::runtime_error("child inspect");
        if (s_->metadata_error)
            return Observation<BasicMetadata>::unavailable(s_->metadata_error->state, s_->metadata_error->error);
        BasicMetadata m;
        if (!s_->missing_child_kind)
            m.kind = Observation<ObjectKind>::available(s_->child_kind);
        if (!s_->missing_child_id) {
            auto id = fixture_identity(s_->foreign_child ? AdapterScope{} : s_->scope, s_->child_id);
            if (s_->unknown_child_scheme)
                id.scheme = IdentityScheme::Unknown;
            m.identity = Observation<ObjectIdentity>::available(std::move(id));
        }
        if (!s_->child_extra_error.empty())
            m.logical_size = Observation<std::uint64_t>::unavailable(ObservationState::Denied,
                                                                     {ErrorDomain::Posix, 13, s_->child_extra_error});
        if (s_->child_identity_error)
            m.identity = Observation<ObjectIdentity>::unavailable(s_->child_identity_error->state,
                                                                  s_->child_identity_error->error);
        if (s_->child_kind_error)
            m.kind = Observation<ObjectKind>::unavailable(s_->child_kind_error->state, s_->child_kind_error->error);
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
    DirectoryIndexSortLimits sort_limits{{1, 4096}, 1};
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
            model, factory, budget, generation, limits, sort_limits);
    }
    void released() {
        CK_CHECK(s->live == 0 && s->child_live == 0 && budget->charged_files() == 0 && budget->charged_bytes() == 0);
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
    while (job.state() == IndexedPaneDataJobState::Sorting || job.state() == IndexedPaneDataJobState::Indexing ||
           job.state() == IndexedPaneDataJobState::CountingMarks) {
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
        pane.begin_listing(PaneOrder{static_cast<PaneSortKey>(-1), false, false});
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
        IndexedPaneDataJob invalid(listing, PaneOrder{static_cast<PaneSortKey>(-1), false, false});
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
CK_TEST(indexed_pane_full_view_filters_hidden_rows_and_preserves_hidden_focus_until_restored) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto before = page(pane);
    const auto key = before->rows()[0].id;
    auto view = pane.data()->view();
    view.show_hidden = false;
    view.display_substring = "B";
    auto target = take(pane.begin_view_change(pane.state_stamp(), view));
    IndexedPaneDataJob filtered(pane.data(), target, view, {{1, 4096}, 1}, {1, 4096});
    finish(filtered);
    CK_CHECK(pane.publish(take(filtered.completed())) == IndexedPanePublish::Applied);
    auto visible = page(pane);
    CK_CHECK(visible->total_rows() == 3 && visible->visible_rows() == 1 && visible->rows()[0].display_name == "b" &&
             visible->end());
    CK_CHECK(pane.focused() == key && !pane.focused_position() && !visible->rows()[0].focused);
    CK_CHECK(pane.focus(visible->stamp(), key) == IndexedPaneFocus::InvalidTarget && pane.accepts(visible->stamp()));
    view.show_hidden = true;
    view.display_substring.clear();
    target = take(pane.begin_view_change(pane.state_stamp(), view));
    IndexedPaneDataJob restored(pane.data(), target, view);
    finish(restored);
    pane.publish(take(restored.completed()));
    CK_CHECK(pane.focused() == key && pane.focused_position() == 0 && page(pane)->rows()[0].focused);
}
CK_TEST(indexed_pane_new_default_view_hides_only_observed_hidden_and_all_excluded_is_not_empty_dataset) {
    Fixture f;
    IndexedPane pane;
    PaneView view;
    view.folders_first = false;
    auto request = pane.begin_listing(view);
    IndexedPaneDataJob initial(collect(f, request), view);
    finish(initial);
    pane.publish(take(initial.completed()));
    CK_CHECK(pane.rows() == 3 && pane.visible_rows() == 2 && page(pane)->rows()[0].display_name == "b" &&
             pane.focused_position() == 0);
    view.display_substring = "absent";
    request = take(pane.begin_view_change(pane.state_stamp(), view));
    IndexedPaneDataJob none(pane.data(), request, view);
    finish(none);
    pane.publish(take(none.completed()));
    auto empty = page(pane);
    CK_CHECK(empty->rows().empty() && empty->total_rows() == 3 && empty->visible_rows() == 0 && empty->end() &&
             !pane.focused_position());
}
CK_TEST(indexed_pane_view_result_binds_every_filter_flag_and_charges_filter_text) {
    Fixture f;
    IndexedPane pane;
    pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending));
    auto view = pane.data()->view();
    view.display_substring = "a";
    auto request = take(pane.begin_view_change(pane.state_stamp(), view));
    auto wrong = view;
    wrong.ascii_case_sensitive = true;
    IndexedPaneDataJob invalid(pane.data(), request, wrong);
    finish(invalid);
    CK_CHECK(pane.publish(take(invalid.completed())) == IndexedPanePublish::InvalidData);
    IndexedPaneDataJob valid(pane.data(), request, view);
    finish(valid);
    pane.publish(take(valid.completed()));
    auto p = page(pane);
    CK_CHECK(p->text_bytes() == 3 && p->view().display_substring == "a");
    CK_CHECK(pane.capture_page(pane.state_stamp(), 0, {{1, 2}, 4096}).state == IndexedPaneCaptureState::LimitExceeded);
    auto stamp = pane.state_stamp();
    view.display_substring = std::string(4097, 'x');
    bool threw = false;
    try {
        pane.begin_view_change(stamp, view);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && pane.accepts(stamp));
}

namespace {
void initialize(IndexedPane &pane, Fixture &f) {
    CK_CHECK(pane.publish(prepare(collect(f, pane.begin_listing(ascending)), ascending)) ==
             IndexedPanePublish::Applied);
}
void finish(IndexedPaneMarkJob &job) {
    std::size_t steps = 0;
    while (job.state() == IndexedMarkJobState::Running) {
        if (++steps > 20000)
            throw std::runtime_error("mark step budget");
        job.advance();
    }
}
void mark(IndexedPane &pane, const IndexedMarkCommand &command) {
    auto job = take(pane.begin_mark_change(pane.state_stamp(), command, 1));
    finish(*job);
    CK_CHECK(pane.publish_marks(take(job->completed())) == IndexedPanePublish::Applied);
}
void view_change(IndexedPane &pane, const PaneView &view) {
    auto request = take(pane.begin_view_change(pane.state_stamp(), view));
    IndexedPaneDataJob job(pane.data(), request, view, {}, {1, 4096}, 1024 * 1024, pane.marks());
    finish(job);
    CK_CHECK(pane.publish(take(job.completed())) == IndexedPanePublish::Applied);
}
} // namespace
CK_TEST(indexed_marks_single_range_visible_invert_clear_are_atomic_and_owner_free) {
    Fixture f;
    std::shared_ptr<const IndexedPanePage> saved;
    {
        IndexedPane pane;
        initialize(pane, f);
        auto original = page(pane);
        const auto first = original->rows()[0].id, last = original->rows()[2].id;
        auto job = take(pane.begin_mark_change(original->stamp(), IndexedMarkRow{first, true}, 1));
        CK_CHECK(!original->stamp().current() && pane.marking_pending());
        auto during = page(pane);
        CK_CHECK(during->marking_pending() && !during->rows()[0].marked && during->selection().candidates == 0);
        CK_CHECK(pane.focus(during->stamp(), last) == IndexedPaneFocus::Unavailable);
        finish(*job);
        CK_CHECK(!page(pane)->rows()[0].marked);
        CK_CHECK(pane.publish_marks(take(job->completed())) == IndexedPanePublish::Applied);
        CK_CHECK(pane.selection().marks->total == 1 && pane.selection().candidates == 1);
        CK_CHECK(pane.selection().origin == PaneSourcesOrigin::Marks && page(pane)->rows()[0].marked);
        job.reset();
        mark(pane, IndexedMarkRange{last, first, true});
        CK_CHECK(pane.selection().marks->total == 3);
        mark(pane, IndexedInvertVisible{});
        CK_CHECK(!pane.marks() && pane.selection().origin == PaneSourcesOrigin::Cursor);
        mark(pane, IndexedMarkVisible{true});
        saved = page(pane);
        CK_CHECK(saved->selection().marks->total == 3 && saved->rows()[2].marked);
        mark(pane, IndexedClearMarks{});
        CK_CHECK(!pane.marks() && !page(pane)->rows()[0].marked && !saved->stamp().current());
    }
    f.released();
    CK_CHECK(saved->rows()[2].marked && saved->selection().marks->total == 3);
}
CK_TEST(indexed_marks_filtering_counts_hidden_marks_blocks_candidates_and_restores_ids) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    auto before = page(pane);
    mark(pane, IndexedMarkVisible{true});
    const auto revision = pane.marks()->revision();
    auto view = pane.data()->view();
    view.display_substring = "B";
    view_change(pane, view);
    const auto counts = pane.selection();
    CK_CHECK(counts.marks->total == 3 && counts.marks->visible == 1 && counts.marks->hidden == 2);
    CK_CHECK(counts.state == PaneSourcesState::HiddenMarksNeedReview && counts.candidates == 0);
    CK_CHECK(pane.marks()->revision() == revision && page(pane)->rows()[0].marked);
    mark(pane, IndexedMarkVisible{false});
    CK_CHECK(pane.selection().marks->total == 2 && pane.selection().marks->visible == 0);
    mark(pane, IndexedInvertVisible{});
    CK_CHECK(pane.selection().marks->total == 3 && pane.selection().marks->visible == 1);
    view.display_substring = "none";
    view_change(pane, view);
    CK_CHECK(page(pane)->end() && pane.selection().marks->hidden == 3 && pane.selection().candidates == 0);
    view.display_substring.clear();
    view.descending = true;
    view_change(pane, view);
    auto restored = page(pane);
    CK_CHECK(restored->rows()[2].id == before->rows()[0].id && restored->rows()[2].marked);
    CK_CHECK(pane.selection().marks->hidden == 0 && pane.selection().candidates == 3);
}
CK_TEST(indexed_marks_invalid_targets_and_quota_reject_without_changing_valid_state) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    auto before = page(pane);
    const auto bytes = f.budget->charged_bytes();
    const auto files = f.budget->charged_files();
    CK_CHECK(!pane.begin_mark_change(before->stamp(), IndexedMarkRow{{AdapterScope{}, 1}, true}).value());
    CK_CHECK(!pane.begin_mark_change(before->stamp(), IndexedMarkRow{{before->rows()[0].id.scope, 0}, true}).value());
    CK_CHECK(!pane.begin_mark_change(before->stamp(), IndexedMarkVisible{true}, 1, 575).value());
    bool threw = false;
    try {
        pane.begin_mark_change(before->stamp(), IndexedMarkVisible{true}, 0);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && pane.accepts(before->stamp()));
    CK_CHECK(pane.accepts(before->stamp()) && !pane.marking_pending() && !pane.marks());
    CK_CHECK(f.budget->charged_bytes() == bytes && f.budget->charged_files() == files);
    auto view = pane.data()->view();
    view.display_substring = "b";
    view_change(pane, view);
    auto stamp = pane.state_stamp();
    CK_CHECK(
        !pane.begin_mark_change(stamp, IndexedMarkRange{before->rows()[0].id, before->rows()[1].id, true}).value());
    CK_CHECK(pane.accepts(stamp) && !pane.marking_pending());
}
CK_TEST(indexed_marks_cancel_failure_and_stale_completion_preserve_published_snapshot) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkVisible{true});
    const auto revision = pane.marks()->revision();
    const auto bytes = f.budget->charged_bytes();
    const auto files = f.budget->charged_files();
    auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}, 1));
    job->advance();
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(job->advance(stop.get_token()) == IndexedMarkJobState::Cancelled);
    CK_CHECK(pane.cancel_marks(job->stamp()) && !pane.cancel_marks(job->stamp()));
    CK_CHECK(pane.marks()->revision() == revision && pane.selection().marks->total == 3);
    CK_CHECK(f.budget->charged_bytes() == bytes && f.budget->charged_files() == files);
    f.c->write_error = UnavailableObservation{ObservationState::Failed, {ErrorDomain::Posix, 28, "mark ENOSPC"}};
    job = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}));
    finish(*job);
    CK_CHECK(job->state() == IndexedMarkJobState::Failed && job->failure()->error.code == 28);
    CK_CHECK(pane.fail_marks(job->stamp(), *job->failure()));
    CK_CHECK(page(pane)->mark_error()->error.operation == "mark ENOSPC" && pane.marks()->revision() == revision);
    auto full = page(pane);
    CK_CHECK(pane.capture_page(full->stamp(), 0, {{3, full->text_bytes() - 1}, 4096}).state ==
             IndexedPaneCaptureState::LimitExceeded);
    f.c->write_error.reset();
    job = take(pane.begin_mark_change(pane.state_stamp(), IndexedClearMarks{}));
    finish(*job);
    auto request = pane.begin_listing(ascending);
    CK_CHECK(!pane.marking_pending() && pane.publish_marks(take(job->completed())) == IndexedPanePublish::Stale);
    CK_CHECK(pane.cancel_listing(request) && pane.marks()->revision() == revision);
    initialize(pane, f);
    CK_CHECK(!pane.marks() && pane.selection().marks->total == 0);
}
CK_TEST(indexed_marks_corruption_is_never_treated_as_clear_and_explicit_clear_recovers) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkVisible{true});
    auto file = f.c->files.back();
    CK_CHECK(file->data.size() == 576);
    file->data[60] ^= std::byte{1};
    auto stamp = pane.state_stamp();
    auto failed = pane.capture_page(stamp, 0);
    CK_CHECK(failed.state == IndexedPaneCaptureState::Failed && !failed.page && !stamp.current());
    CK_CHECK(pane.selection().state == PaneSourcesState::SelectionInvalidated && !pane.selection().marks);
    CK_CHECK(!pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}).value());
    mark(pane, IndexedClearMarks{});
    CK_CHECK(!pane.marks() && page(pane)->selection().origin == PaneSourcesOrigin::Cursor);
}
CK_TEST(indexed_marks_prepare_and_capture_discard_replies_superseded_during_IO) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    const auto id = page(pane)->rows()[0].id;
    bool triggered = false;
    PaneListingRequest request;
    for (auto &file : f.c->files)
        if (!file->closed)
            file->on_read = [&] {
                if (!triggered) {
                    triggered = true;
                    request = pane.begin_listing(descending);
                }
            };
    CK_CHECK(!pane.begin_mark_change(pane.state_stamp(), IndexedMarkRow{id, true}).value());
    CK_CHECK(pane.listing_pending() && !pane.marking_pending() && !pane.marks());
    for (auto &file : f.c->files)
        file->on_read = {};
    pane.cancel_listing(request);
    mark(pane, IndexedMarkVisible{true});
    triggered = false;
    f.c->files.back()->on_read = [&] {
        if (!triggered) {
            triggered = true;
            request = pane.begin_listing(descending);
        }
    };
    const auto captured = pane.capture_page(pane.state_stamp(), 0);
    CK_CHECK(captured.state == IndexedPaneCaptureState::Stale && !captured.page && pane.listing_pending());
}
CK_TEST(indexed_marks_multiblock_boundaries_tail_bits_and_copy_on_write_quota) {
    Fixture f({});
    f.sort_limits = {{128, 64 * 1024}, 64};
    f.budget = std::make_shared<IndexStorageBudget>(16 * 1024 * 1024, 32);
    for (std::size_t n = 0; n < 4101; ++n)
        f.s->names.push_back(*NativeName::from_posix("entry" + std::to_string(n)));
    IndexedPane pane;
    initialize(pane, f);
    const auto scope = pane.data()->row_scope();
    mark(pane, IndexedMarkRow{{scope, 4096}, true});
    const auto bytes = f.budget->charged_bytes();
    auto old = pane.marks();
    CK_CHECK(take(old->contains({scope, 4096})) && !take(old->contains({scope, 4097})));
    mark(pane, IndexedMarkRow{{scope, 4097}, true});
    CK_CHECK(pane.selection().marks->total == 2 && f.budget->charged_bytes() == bytes + 1152);
    CK_CHECK(!take(old->contains({scope, 4097})) && take(pane.marks()->contains({scope, 4097})));
    old.reset();
    CK_CHECK(f.budget->charged_bytes() == bytes);
    mark(pane, IndexedMarkVisible{true});
    CK_CHECK(pane.selection().marks->total == 4101 && f.c->files.back()->data.size() == 1152);
    mark(pane, IndexedInvertVisible{});
    CK_CHECK(!pane.marks() && f.budget->charged_bytes() == bytes - 1152);
}
CK_TEST(indexed_marks_recount_cancellation_and_wrong_revision_cannot_publish_new_view) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkVisible{true});
    auto view = pane.data()->view();
    view.display_substring = "b";
    auto request = take(pane.begin_view_change(pane.state_stamp(), view));
    IndexedPaneDataJob missing(pane.data(), request, view);
    finish(missing);
    CK_CHECK(pane.publish(take(missing.completed())) == IndexedPanePublish::InvalidData);
    IndexedPaneDataJob job(pane.data(), request, view, {}, {1, 4096}, 1024 * 1024, pane.marks());
    std::size_t steps = 0;
    while (job.state() == IndexedPaneDataJobState::Sorting || job.state() == IndexedPaneDataJobState::Indexing) {
        CK_CHECK(++steps < 1000);
        job.advance();
    }
    CK_CHECK(job.state() == IndexedPaneDataJobState::CountingMarks);
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(job.advance(stop.get_token()) == IndexedPaneDataJobState::Cancelled);
    CK_CHECK(pane.cancel_listing(request) && pane.selection().marks->visible == 3);
    CK_CHECK(page(pane)->rows().size() == 3);
}
CK_TEST(indexed_marks_short_transfers_and_zero_progress_never_publish_partial_bitmap) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedMarkVisible{true}));
    auto file = f.c->files.back();
    file->write_max = 7;
    finish(*job);
    CK_CHECK(pane.publish_marks(take(job->completed())) == IndexedPanePublish::Applied);
    file->read_max = 5;
    CK_CHECK(page(pane)->rows()[2].marked);
    job.reset();
    auto next = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}));
    f.c->files.back()->write_max = 0;
    finish(*next);
    CK_CHECK(next->state() == IndexedMarkJobState::Failed && next->failure()->error.operation == "marks.write_count");
    pane.fail_marks(next->stamp(), *next->failure());
    CK_CHECK(pane.selection().marks->total == 3 && page(pane)->rows()[0].marked);
    file->read_max = 0;
    auto bad = pane.capture_page(pane.state_stamp(), 0);
    CK_CHECK(bad.state == IndexedPaneCaptureState::Failed &&
             pane.selection().state == PaneSourcesState::SelectionInvalidated);
}
CK_TEST(indexed_marks_real_write_error_wins_over_simultaneous_cancellation) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedMarkVisible{true}));
    std::stop_source stop;
    auto file = f.c->files.back();
    file->write_error = UnavailableObservation{ObservationState::Failed, {ErrorDomain::Win32, 112, "disk full"}};
    file->on_write = [&] { stop.request_stop(); };
    CK_CHECK(job->advance(stop.get_token()) == IndexedMarkJobState::Failed);
    CK_CHECK(job->failure()->error.domain == ErrorDomain::Win32 && job->failure()->error.code == 112);
    CK_CHECK(pane.fail_marks(job->stamp(), *job->failure()) && !pane.marks());
}
CK_TEST(indexed_marks_cancel_each_boundary_preserves_previous_counts_and_releases_charges) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkRow{page(pane)->rows()[0].id, true});
    const auto bytes = f.budget->charged_bytes();
    const auto files = f.budget->charged_files();
    for (std::size_t boundary = 0; boundary < 3; ++boundary) {
        auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}, 1));
        for (std::size_t n = 0; n < boundary; ++n)
            CK_CHECK(job->advance() == IndexedMarkJobState::Running);
        std::stop_source stop;
        stop.request_stop();
        CK_CHECK(job->advance(stop.get_token()) == IndexedMarkJobState::Cancelled);
        CK_CHECK(pane.cancel_marks(job->stamp()) && pane.selection().marks->total == 1);
        CK_CHECK(f.budget->charged_bytes() == bytes && f.budget->charged_files() == files);
    }
    auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}));
    CK_CHECK(job->abandon() && !job->abandon());
    CK_CHECK(pane.cancel_marks(job->stamp()) && pane.selection().marks->total == 1);
}
CK_TEST(indexed_marks_factory_failures_and_exceptions_release_reservations) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    const auto bytes = f.budget->charged_bytes();
    const auto files = f.budget->charged_files();
    auto stamp = pane.state_stamp();
    f.c->create_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "mark create"}};
    auto denied = pane.begin_mark_change(stamp, IndexedMarkVisible{true});
    CK_CHECK(!denied.value() && denied.unavailable_detail()->error.code == 13 && pane.accepts(stamp));
    f.c->create_error.reset();
    f.c->throw_create = true;
    bool threw = false;
    try {
        pane.begin_mark_change(stamp, IndexedMarkVisible{true});
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CK_CHECK(threw && !stamp.current() && !pane.marking_pending() && !pane.marks());
    CK_CHECK(f.budget->charged_bytes() == bytes && f.budget->charged_files() == files);
}
CK_TEST(indexed_marks_detect_crc_valid_nonzero_tail_padding_and_preserve_native_read_error) {
    for (const bool native_error : {false, true}) {
        Fixture f;
        IndexedPane pane;
        initialize(pane, f);
        mark(pane, IndexedMarkVisible{true});
        auto file = f.c->files.back();
        if (native_error)
            file->read_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "mark read"}};
        else {
            file->data[571] = std::byte{1};
            const auto crc = detail::directory_index_crc32c(std::span<const std::byte>(file->data).first(572));
            for (std::size_t n = 0; n < 4; ++n)
                file->data[572 + n] = static_cast<std::byte>((crc >> (8 * n)) & 255);
        }
        auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}));
        finish(*job);
        CK_CHECK(job->state() == IndexedMarkJobState::Failed);
        CK_CHECK(job->failure()->error.operation == (native_error ? "mark read" : "marks.padding"));
        CK_CHECK(pane.fail_marks(job->stamp(), *job->failure()));
        CK_CHECK(pane.selection().state == PaneSourcesState::SelectionInvalidated && !pane.selection().marks);
    }
}
CK_TEST(indexed_marks_inconsistent_popcount_invalidates_snapshot_during_change_and_recount) {
    for (const bool recount : {false, true}) {
        Fixture f;
        IndexedPane pane;
        initialize(pane, f);
        mark(pane, IndexedMarkVisible{true});
        auto file = f.c->files.back();
        file->data[60] ^= std::byte{1};
        const auto crc = detail::directory_index_crc32c(std::span<const std::byte>(file->data).first(572));
        for (std::size_t n = 0; n < 4; ++n)
            file->data[572 + n] = static_cast<std::byte>((crc >> (8 * n)) & 255);
        if (recount) {
            auto view = pane.data()->view();
            view.display_substring = "b";
            auto request = take(pane.begin_view_change(pane.state_stamp(), view));
            IndexedPaneDataJob job(pane.data(), request, view, {}, {1, 4096}, 1024 * 1024, pane.marks());
            finish(job);
            CK_CHECK(job.state() == IndexedPaneDataJobState::Failed &&
                     job.failure()->error.operation == "marks.recount_total");
            CK_CHECK(pane.fail_listing(request, *job.failure()));
        } else {
            auto job = take(pane.begin_mark_change(pane.state_stamp(), IndexedInvertVisible{}));
            finish(*job);
            CK_CHECK(job->state() == IndexedMarkJobState::Failed && job->failure()->error.operation == "marks.counts");
            CK_CHECK(pane.fail_marks(job->stamp(), *job->failure()));
        }
        CK_CHECK(pane.marks()->state() == DirectoryIndexMarksState::Failed);
        CK_CHECK(pane.selection().state == PaneSourcesState::SelectionInvalidated && !pane.selection().marks);
        mark(pane, IndexedClearMarks{});
        CK_CHECK(!pane.marks() && page(pane)->rows().size() == 3);
    }
}

namespace {
std::size_t finish(IndexedPaneSourceJob &job) {
    std::size_t steps = 0;
    while (job.state() == IndexedSourceJobState::Running) {
        if (++steps > 10000)
            throw std::runtime_error("source job step budget");
        job.advance();
    }
    return steps;
}
} // namespace
CK_TEST(indexed_sources_cursor_uses_new_binding_ids_and_result_pins_no_index_files) {
    Fixture f;
    IndexedPaneSourceJob::Result saved;
    {
        IndexedPane pane;
        initialize(pane, f);
        const auto original = page(pane)->rows()[0].id;
        const auto opens = f.s->opened;
        auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
        CK_CHECK(f.s->opened == opens);
        CK_CHECK(finish(*job) == 3);
        saved = take(job->completed());
        CK_CHECK(pane.accepts_sources(*saved) && saved->review_required() && saved->sources().size() == 1);
        const auto &source = saved->sources()[0];
        CK_CHECK(source.discovery_row() == original && source.binding_row().scope != original.scope);
        CK_CHECK(source.binding_row().number == 1 && source.held()->name().display_name() == ".a");
        CK_CHECK(source.cached_identity_match() == IdentityComparison::Same && saved->native_name_bytes() == 2);
        CK_CHECK(f.s->opened == opens + 1 && f.s->child_live == 1);
        auto independently_opened = take(HeldChild::observe(std::make_shared<Cursor>(f.s), source.held()->name()));
        CK_CHECK(!source.held()->continues(*independently_opened));
    }
    CK_CHECK(!saved->stamp().current() && f.s->live == 1 && f.s->child_live == 1);
    CK_CHECK(f.budget->charged_bytes() == 0 && f.budget->charged_files() == 0);
    CK_CHECK(saved->sources()[0].held()->inspect_basic().value());
    saved.reset();
    f.released();
}
CK_TEST(indexed_sources_marks_are_complete_and_hidden_or_oversized_selections_reject_before_IO) {
    Fixture f;
    IndexedPane pane, empty;
    CK_CHECK(!empty.begin_source_acquisition(empty.state_stamp()).value());
    initialize(pane, f);
    mark(pane, IndexedMarkVisible{true});
    const auto opens = f.s->opened;
    IndexedSourceLimits limit;
    limit.held_entries = 2;
    CK_CHECK(!pane.begin_source_acquisition(pane.state_stamp(), limit).value() && f.s->opened == opens);
    limit.held_entries = 3;
    limit.scan_rows = 1;
    auto job = take(pane.begin_source_acquisition(pane.state_stamp(), limit));
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(result->sources().size() == 3 && f.s->opened == opens + 3);
    for (std::size_t n = 0; n < 3; ++n)
        CK_CHECK(result->sources()[n].discovery_row().number == n + 1 &&
                 result->sources()[n].binding_row().number == n + 1);
    auto view = pane.data()->view();
    view.display_substring = "b";
    view_change(pane, view);
    const auto after = f.s->opened;
    CK_CHECK(!pane.accepts_sources(*result) && !job->completed().value());
    CK_CHECK(!pane.begin_source_acquisition(pane.state_stamp()).value() && f.s->opened == after);
    auto request = pane.begin_listing(ascending);
    CK_CHECK(!pane.begin_source_acquisition(pane.state_stamp()).value() && f.s->opened == after);
    pane.cancel_listing(request);
}
CK_TEST(indexed_sources_changed_cached_identity_or_kind_always_requires_fresh_review) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    f.s->child_id = 55;
    f.s->child_kind = ObjectKind::Directory;
    auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(result->review_required() &&
             result->sources()[0].cached_identity_match() == IdentityComparison::Different);
    CK_CHECK(*result->sources()[0].discovery_metadata().value()->kind.value() == ObjectKind::RegularFile);
    CK_CHECK(*result->sources()[0].held()->metadata().kind.value() == ObjectKind::Directory);
}
CK_TEST(indexed_sources_failure_after_private_prefix_releases_all_children_and_preserves_native_error) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        IndexedPane pane;
        initialize(pane, f);
        mark(pane, IndexedMarkVisible{true});
        auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
        while (f.s->child_live == 0 && job->state() == IndexedSourceJobState::Running)
            job->advance();
        CK_CHECK(f.s->child_live == 1);
        std::stop_source stop;
        const UnavailableObservation denied{ObservationState::Denied, {ErrorDomain::Posix, 13, "acquire denied"}};
        if (fault == 0)
            f.s->metadata_error = denied;
        if (fault == 1)
            f.s->child_identity_error = denied;
        if (fault == 2)
            f.s->child_kind_error = denied;
        f.s->on_child_inspect = [&] { stop.request_stop(); };
        while (job->state() == IndexedSourceJobState::Running)
            job->advance(stop.get_token());
        CK_CHECK(job->state() == IndexedSourceJobState::Failed && job->failure()->error.code == 13);
        CK_CHECK(!job->completed().value() && f.s->child_live == 0 && pane.selection().marks->total == 3);
    }
}
CK_TEST(indexed_sources_cancellation_at_every_boundary_releases_private_handles_without_partial_result) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkVisible{true});
    auto baseline = take(pane.begin_source_acquisition(pane.state_stamp()));
    const auto steps = finish(*baseline);
    CK_CHECK(baseline->state() == IndexedSourceJobState::Complete);
    baseline->abandon();
    const auto bytes = f.budget->charged_bytes();
    for (std::size_t boundary = 0; boundary < steps; ++boundary) {
        auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
        for (std::size_t n = 0; n < boundary; ++n)
            job->advance();
        std::stop_source stop;
        stop.request_stop();
        CK_CHECK(job->advance(stop.get_token()) == IndexedSourceJobState::Cancelled);
        CK_CHECK(!job->completed().value() && f.s->child_live == 0 && f.budget->charged_bytes() == bytes);
    }
}
CK_TEST(indexed_sources_new_request_during_native_read_wins_and_completed_results_revoke) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
    PaneListingRequest request;
    f.s->on_child_inspect = [&] { request = pane.begin_listing(descending); };
    finish(*job);
    CK_CHECK(job->state() == IndexedSourceJobState::Stale && !job->completed().value() && f.s->child_live == 0);
    f.s->on_child_inspect = {};
    pane.cancel_listing(request);
    job = take(pane.begin_source_acquisition(pane.state_stamp()));
    finish(*job);
    auto result = take(job->completed());
    auto rows = page(pane);
    pane.focus(rows->stamp(), rows->rows()[1].id);
    CK_CHECK(!pane.accepts_sources(*result) && !job->completed().value() && !result->stamp().current());
}
CK_TEST(indexed_sources_unqualified_parent_or_child_cannot_become_acquired_sources) {
    for (int fault = 0; fault < 7; ++fault) {
        Fixture f;
        IndexedPane pane;
        initialize(pane, f);
        if (fault == 0)
            f.s->root_id = false;
        if (fault == 1)
            f.s->scope_changed = true;
        if (fault == 2)
            f.s->missing_child_id = true;
        if (fault == 3)
            f.s->foreign_child = true;
        if (fault == 4)
            f.s->unknown_child_scheme = true;
        if (fault == 5)
            f.s->missing_child_kind = true;
        if (fault == 6)
            f.s->child_kind = static_cast<ObjectKind>(-1);
        auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
        finish(*job);
        CK_CHECK(job->state() == IndexedSourceJobState::Failed && !job->completed().value() && f.s->child_live == 0);
    }
}
CK_TEST(indexed_sources_name_wire_and_diagnostic_limits_fail_without_truncating_selection) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        IndexedPane pane;
        initialize(pane, f);
        const auto opens = f.s->opened;
        IndexedSourceLimits limits;
        if (fault == 0)
            limits.native_name_bytes = 1;
        if (fault == 1)
            limits.wire_bytes = 16;
        if (fault == 2) {
            limits.diagnostic_bytes = 1;
            f.s->child_extra_error = "fresh diagnostic";
        }
        auto job = take(pane.begin_source_acquisition(pane.state_stamp(), limits));
        finish(*job);
        CK_CHECK(job->state() == IndexedSourceJobState::Failed && !job->completed().value() && f.s->child_live == 0);
        CK_CHECK(f.s->opened == opens + (fault == 2 ? 1 : 0));
        CK_CHECK(pane.selection().state == PaneSourcesState::Ready);
    }
}
CK_TEST(indexed_sources_mark_corruption_revokes_snapshot_and_stamp_and_allows_explicit_recovery) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkVisible{true});
    auto file = f.c->files.back();
    file->data[60] ^= std::byte{1};
    const auto crc = detail::directory_index_crc32c(std::span<const std::byte>(file->data).first(572));
    for (std::size_t n = 0; n < 4; ++n)
        file->data[572 + n] = static_cast<std::byte>((crc >> (8 * n)) & 255);
    auto old = pane.state_stamp();
    auto job = take(pane.begin_source_acquisition(old));
    finish(*job);
    CK_CHECK(job->state() == IndexedSourceJobState::Failed &&
             job->failure()->error.operation == "indexed_sources.mark_count");
    CK_CHECK(!old.current() && pane.selection().state == PaneSourcesState::SelectionInvalidated &&
             f.s->child_live == 0);
    CK_CHECK(pane.accepts(pane.state_stamp()));
    mark(pane, IndexedClearMarks{});
    CK_CHECK(page(pane)->selection().state == PaneSourcesState::Ready);
}
CK_TEST(indexed_sources_parent_change_and_native_exceptions_discard_private_batch) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        IndexedPane pane;
        initialize(pane, f);
        auto job = take(pane.begin_source_acquisition(pane.state_stamp()));
        if (fault == 0)
            f.s->on_child_inspect = [&] { f.s->changed = true; };
        if (fault == 1)
            f.s->throw_child_inspect = true;
        if (fault == 2)
            f.s->on_child_inspect = [&] { f.s->throw_inspect = true; };
        bool threw = false;
        try {
            finish(*job);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw == (fault != 0) && job->state() == IndexedSourceJobState::Failed);
        CK_CHECK(!job->completed().value() && f.s->child_live == 0);
        if (fault == 0)
            CK_CHECK(job->failure()->state == ObservationState::Changed);
    }
}
CK_TEST(indexed_sources_use_lossless_native_names_and_invalid_limits_leave_stamp_current) {
    Fixture f({"a"});
    f.s->names[0] = *NativeName::from_posix(std::string("a\n\xFF", 3));
    IndexedPane pane;
    initialize(pane, f);
    auto stamp = pane.state_stamp();
    IndexedSourceLimits invalid;
    invalid.scan_rows = 0;
    bool threw = false;
    try {
        pane.begin_source_acquisition(stamp, invalid);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && pane.accepts(stamp));
    auto job = take(pane.begin_source_acquisition(stamp));
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(result->sources()[0].held()->name() == f.s->names[0] && result->native_name_bytes() == 3);
}
CK_TEST(indexed_sources_excluded_mark_contradiction_invalidates_snapshot_instead_of_reusing_cached_counts) {
    Fixture f;
    IndexedPane pane;
    initialize(pane, f);
    mark(pane, IndexedMarkRow{page(pane)->rows()[1].id, true});
    auto view = pane.data()->view();
    view.display_substring = "b";
    view_change(pane, view);
    CK_CHECK(pane.selection().candidates == 1);
    std::shared_ptr<File> bitmap;
    for (auto &file : f.c->files)
        if (!file->closed && file->data.size() == 576)
            bitmap = file;
    CK_CHECK(bitmap);
    bitmap->data[60] ^= std::byte{5}; // original row3 -> row1, preserving popcount
    const auto crc = detail::directory_index_crc32c(std::span<const std::byte>(bitmap->data).first(572));
    for (std::size_t n = 0; n < 4; ++n)
        bitmap->data[572 + n] = static_cast<std::byte>((crc >> (8 * n)) & 255);
    const auto stamp = pane.state_stamp();
    const auto opens = f.s->opened;
    auto job = take(pane.begin_source_acquisition(stamp));
    finish(*job);
    CK_CHECK(job->state() == IndexedSourceJobState::Failed &&
             job->failure()->error.operation == "indexed_sources.excluded_selection");
    CK_CHECK(!stamp.current() && f.s->opened == opens &&
             pane.selection().state == PaneSourcesState::SelectionInvalidated);
    mark(pane, IndexedClearMarks{});
    CK_CHECK(!pane.marks() && pane.selection().candidates == 0);
}
