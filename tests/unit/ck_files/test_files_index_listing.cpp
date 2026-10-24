// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_listing_job.hpp"
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
} // namespace
CK_TEST(index_listing_collects_native_End_and_preserves_original_rows_in_name_order) {
    Fixture f;
    auto job = f.job();
    CK_CHECK(!job->completed().value());
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(result->request().scope == f.request.scope && result->request().number == 42);
    CK_CHECK(result->source_scope() == f.s->scope && result->directory_pin() && f.s->live == 1);
    CK_CHECK(result->before().value() && result->after().value() && result->store()->rows() == 3);
    auto cursor = take(result->store()->begin_scan());
    auto page = take(result->store()->read_page(cursor));
    CK_CHECK(page.end && page.rows.size() == 3);
    CK_CHECK(page.rows[0].record.row_id == 2 && *page.rows[0].record.name.posix_bytes() == ".a" &&
             *page.rows[0].record.hidden.value());
    CK_CHECK(page.rows[1].record.row_id == 3 && page.rows[2].record.row_id == 1 &&
             !*page.rows[1].record.hidden.value());
    for (const auto &row : page.rows)
        CK_CHECK(row.record.metadata.value()->identity.value()->scope == f.s->scope);
    const auto reads = f.s->reads;
    CK_CHECK(job->advance() == DirectoryIndexListingState::Complete && f.s->reads == reads);
    job.reset();
    CK_CHECK(f.s->live == 1);
    result.reset();
    f.released();
}
CK_TEST(index_listing_explicit_source_scope_survives_unavailable_directory_ID) {
    Fixture f;
    f.s->root_id = false;
    auto job = f.job();
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(!result->directory().value() && !result->before().value()->identity.value());
    auto page = take(result->store()->read_page(take(result->store()->begin_scan())));
    CK_CHECK(page.rows[0].record.metadata.value()->identity.value()->scope == f.s->scope);
    job.reset();
    result.reset();
    f.released();
}
CK_TEST(index_listing_unavailable_namespace_is_preserved_without_creating_cache) {
    for (auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        Fixture f;
        f.s->scope_error = UnavailableObservation{state, {ErrorDomain::Posix, 123, "scope"}};
        auto job = f.job();
        CK_CHECK(job->state() == DirectoryIndexListingState::Failed &&
                 job->failure()->error == f.s->scope_error->error);
        CK_CHECK(f.c->files.empty() && f.s->inspections == 0 && !job->completed().value());
        f.released();
    }
}
CK_TEST(index_listing_rejects_duplicate_names_across_sorted_page_boundaries) {
    Fixture f({"same", "z", "same"});
    auto job = f.job();
    finish(*job);
    CK_CHECK(job->state() == DirectoryIndexListingState::Failed &&
             job->failure()->error.operation == "index_listing.duplicate_or_order");
    CK_CHECK(!job->completed().value());
    f.released();
}
CK_TEST(index_listing_cancellation_at_every_advance_releases_cursor_cache_and_sort_runs) {
    Fixture baseline;
    auto complete = baseline.job();
    auto steps = finish(*complete);
    complete.reset();
    baseline.released();
    for (std::size_t at = 0; at < steps; ++at) {
        Fixture f;
        auto job = f.job();
        for (std::size_t i = 0; i < at; ++i)
            job->advance();
        std::stop_source stop;
        stop.request_stop();
        job->advance(stop.get_token());
        CK_CHECK(job->state() == DirectoryIndexListingState::Cancelled && !job->completed().value());
        CK_CHECK(!job->abandon());
        f.released();
    }
}
CK_TEST(index_listing_stop_during_native_read_discards_partial_listing) {
    Fixture f;
    std::stop_source stop;
    f.s->on_read = [&] { stop.request_stop(); };
    auto job = f.job();
    job->advance(stop.get_token());
    CK_CHECK(job->state() == DirectoryIndexListingState::Cancelled && !job->completed().value());
    f.released();
}
CK_TEST(index_listing_detects_directory_timestamp_and_namespace_changes_before_publication) {
    for (int change = 0; change < 2; ++change) {
        Fixture f;
        auto job = f.job();
        while (job->state() != DirectoryIndexListingState::CheckingNames)
            job->advance();
        if (change == 0)
            f.s->changed = true;
        else
            f.s->scope_changed = true;
        finish(*job);
        CK_CHECK(job->state() == DirectoryIndexListingState::Failed && !job->completed().value());
        CK_CHECK(job->failure()->error.operation ==
                 (change == 0 ? "index_listing.directory_changed" : "index_listing.scope_changed"));
        f.released();
    }
}
CK_TEST(index_listing_rejects_foreign_root_and_child_identity_namespaces) {
    for (bool root : {true, false}) {
        Fixture f;
        f.s->foreign_root = root;
        f.s->foreign_child = !root;
        auto job = f.job();
        finish(*job);
        CK_CHECK(job->state() == DirectoryIndexListingState::Failed && !job->completed().value());
        f.released();
    }
}
CK_TEST(index_listing_entry_metadata_errors_remain_records_and_never_disappear) {
    Fixture f;
    f.s->metadata_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Win32, 5, "metadata"}};
    auto job = f.job(StorageModel::DarwinStatFlags);
    finish(*job);
    auto result = take(job->completed());
    auto page = take(result->store()->read_page(take(result->store()->begin_scan())));
    CK_CHECK(page.rows.size() == 3);
    for (const auto &row : page.rows)
        CK_CHECK(row.record.metadata.state() == ObservationState::Denied &&
                 row.record.metadata.unavailable_detail()->error == f.s->metadata_error->error);
    CK_CHECK(*page.rows[0].record.hidden.value() && page.rows[1].record.hidden.state() == ObservationState::Denied);
    job.reset();
    result.reset();
    f.released();
}
CK_TEST(index_listing_enumeration_quota_and_create_errors_publish_no_partial_result) {
    for (int fault = 0; fault < 4; ++fault) {
        Fixture f;
        if (fault == 0)
            f.s->read_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "read"}};
        if (fault == 1)
            f.budget = std::make_shared<IndexStorageBudget>(16, 16);
        if (fault == 2)
            f.budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 1);
        if (fault == 3)
            f.c->create_error = UnavailableObservation{ObservationState::Failed, {ErrorDomain::Posix, 28, "create"}};
        auto job = f.job();
        finish(*job);
        CK_CHECK(job->state() == DirectoryIndexListingState::Failed && !job->completed().value());
        f.released();
    }
}
CK_TEST(index_listing_empty_input_and_abandonment_have_explicit_owner_lifetimes) {
    Fixture f({});
    auto job = f.job();
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(result->store()->rows() == 0);
    CK_CHECK(job->abandon() && !job->abandon() && !job->completed().value());
    CK_CHECK(f.s->live == 1);
    result.reset();
    job.reset();
    f.released();
    Fixture pending;
    auto staging = pending.job();
    staging->advance();
    CK_CHECK(staging->abandon());
    pending.released();
}
CK_TEST(index_listing_exceptions_and_encoding_mismatch_retire_staging) {
    {
        Fixture f;
        auto job = f.job();
        while (job->state() != DirectoryIndexListingState::CheckingNames)
            job->advance();
        f.s->throw_inspect = true;
        bool threw = false;
        try {
            finish(*job);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw && job->state() == DirectoryIndexListingState::Failed && !job->completed().value());
        f.released();
    }
    {
        Fixture f;
        auto job = f.job(StorageModel::WindowsHandleAttributes);
        finish(*job);
        CK_CHECK(job->state() == DirectoryIndexListingState::Failed);
        f.released();
    }
}

CK_TEST(index_listing_directory_metadata_uncertainty_remains_explicit_in_result) {
    Fixture f;
    f.s->root_id = false;
    f.s->root_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "root_metadata"}};
    auto job = f.job();
    finish(*job);
    auto result = take(job->completed());
    CK_CHECK(result->before().state() == ObservationState::Denied &&
             result->after().state() == ObservationState::Denied);
    CK_CHECK(result->before().unavailable_detail()->error == f.s->root_error->error);
    job.reset();
    result.reset();
    f.released();
    Fixture changed;
    auto pending = changed.job();
    while (pending->state() != DirectoryIndexListingState::CheckingNames)
        pending->advance();
    changed.s->root_error = UnavailableObservation{ObservationState::Gone, {ErrorDomain::Posix, 2, "root_gone"}};
    finish(*pending);
    CK_CHECK(pending->state() == DirectoryIndexListingState::Failed &&
             pending->failure()->error == changed.s->root_error->error);
    changed.released();
}
CK_TEST(index_listing_invalid_model_and_generation_are_rejected_without_files) {
    Fixture f;
    bool threw = false;
    try {
        auto job = f.job(static_cast<StorageModel>(-1));
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && f.s->inspections == 0 && f.c->files.empty());
    f.released();
    Fixture g;
    g.generation = {};
    auto invalid = g.job();
    CK_CHECK(invalid->state() == DirectoryIndexListingState::Failed && g.c->files.empty());
    CK_CHECK(invalid->failure()->error.operation == "index_store.context");
    g.released();
}
CK_TEST(index_listing_default_cursor_never_infers_adapter_scope_from_native_identity) {
    class Unscoped final : public DirectoryCursor {
      public:
        Unscoped() : DirectoryCursor(Observation<ObjectIdentity>::available(fixture_identity(AdapterScope{}, 1))) {
        }

      private:
        Observation<DirectoryStep> next_step() override {
            throw std::runtime_error("unexpected read");
        }
    };
    Fixture f;
    DirectoryIndexListingJob job(f.request, std::make_unique<DirectoryListing>(std::make_unique<Unscoped>()),
                                 StorageModel::Unknown, f.factory, f.budget, f.generation);
    CK_CHECK(job.state() == DirectoryIndexListingState::Failed &&
             job.failure()->state == ObservationState::Unsupported);
    CK_CHECK(f.c->files.empty() && !job.completed().value());
    f.released();
}
