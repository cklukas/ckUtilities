// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_completion_index.hpp"
#include "files_location_draft.hpp"
#include "native_index_file.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include <fstream>
#include <map>
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <functional>
namespace {
using namespace ck::files;
using namespace ck::vision;
using Bytes = std::vector<std::byte>;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("sort fixture: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
struct File {
    Bytes data;
    bool closed = false;
    std::size_t read_calls = 0;
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
    explicit ByteFile(std::shared_ptr<File> file) : f(std::move(file)) {}
    ~ByteFile() override {
        f->closed = true;
        f->data.clear();
    }
    Observation<std::uint64_t> size() override { return Observation<std::uint64_t>::available(f->data.size()); }
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
        ++f->read_calls;
        if (f->on_read)
            f->on_read();
        if (f->read_error)
            return Observation<std::size_t>::unavailable(f->read_error->state, f->read_error->error);
        const auto at = static_cast<std::size_t>(offset);
        const auto n = at < f->data.size() ? std::min(bytes.size(), f->data.size() - at) : 0;
        if (n)
            std::copy_n(f->data.begin() + static_cast<std::ptrdiff_t>(at), n, bytes.begin());
        return Observation<std::size_t>::available(n);
    }

  private:
    std::shared_ptr<File> f;
};
class Factory final : public IndexFileFactory {
  public:
    explicit Factory(std::shared_ptr<Control> value) : c(std::move(value)) {}
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
    std::map<NativeName, Observation<BasicMetadata>, NativeNameLess> observations;
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
    explicit Entry(std::shared_ptr<Script> s, NativeName name) : s_(std::move(s)), name_(std::move(name)) {}
    Observation<BasicMetadata> inspect_basic() const override {
        if (const auto it = s_->observations.find(name_); it != s_->observations.end())
            return it->second;
        if (s_->metadata_error)
            return Observation<BasicMetadata>::unavailable(s_->metadata_error->state, s_->metadata_error->error);
        BasicMetadata m;
        m.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        m.identity =
            Observation<ObjectIdentity>::available(fixture_identity(s_->foreign_child ? AdapterScope{} : s_->scope, 2));
        return Observation<BasicMetadata>::available(std::move(m));
    }

  private:
    std::shared_ptr<Script> s_;
    NativeName name_;
};
class Cursor final : public DirectoryCursor {
  public:
    explicit Cursor(std::shared_ptr<Script> s)
        : DirectoryCursor(s->root_id ? Observation<ObjectIdentity>::available(fixture_identity(s->scope, 1))
                                     : Observation<ObjectIdentity>{}),
          s_(std::move(s)) {
        ++s_->live;
    }
    ~Cursor() override { --s_->live; }
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
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName &name, EntryAccess access) const override {
        CK_CHECK(access == EntryAccess::MetadataOnly);
        ++s_->opened;
        return Observation<std::unique_ptr<OpenedEntry>>::available(std::make_unique<Entry>(s_, name));
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
    std::shared_ptr<IndexStorageBudget> budget = std::make_shared<IndexStorageBudget>(32 * 1024 * 1024, 64);
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
            std::make_unique<DirectoryListing>(std::make_unique<Cursor>(s),
                                               DirectoryListingLimits{128, 64 * 1024, 64 * 1024}, clock),
            model, factory, budget, generation, limits, DirectoryIndexSortLimits{{128, 64 * 1024}, 128});
    }
    void released() { CK_CHECK(s->live == 0 && budget->charged_files() == 0 && budget->charged_bytes() == 0); }
};
std::size_t finish(DirectoryIndexListingJob &job) {
    std::size_t steps = 0;
    while (job.state() == DirectoryIndexListingState::Collecting ||
           job.state() == DirectoryIndexListingState::SortingNames ||
           job.state() == DirectoryIndexListingState::CheckingNames) {
        if (++steps > 100000)
            throw std::runtime_error("listing step budget");
        job.advance();
    }
    return steps;
}

FilesLocationCompletionQuery query(std::string_view text = "/root/",
                                   FilesLocationTextSyntax syntax = FilesLocationTextSyntax::Path,
                                   NameEncoding encoding = NameEncoding::PosixBytes) {
    return take(parse_files_location_completion(text, syntax, encoding));
}
std::shared_ptr<const CompletedDirectoryIndex> dataset(Fixture &f, StorageModel model = StorageModel::LinuxStatx) {
    auto job = f.job(model);
    finish(*job);
    return take(job->completed());
}
FilesCompletionIndexJob::Result finish(FilesCompletionIndexJob &job) {
    std::size_t steps = 0;
    while (job.state() == FilesCompletionIndexState::Running) {
        if (++steps > 100000)
            throw std::runtime_error("completion step limit");
        job.advance();
    }
    return take(job.completed());
}
BasicMetadata kind(ObjectKind k) {
    BasicMetadata m;
    m.kind = Observation<ObjectKind>::available(k);
    return m;
}
std::size_t reads(const Fixture &f) {
    std::size_t n = 0;
    for (const auto &p : f.c->files)
        n += p->read_calls;
    return n;
}
void set_read_hook(Fixture &f, std::function<void()> hook) {
    for (const auto &p : f.c->files)
        if (!p->closed)
            p->on_read = hook;
}
} // namespace
CK_TEST(files_completion_index_filters_native_prefixes_in_canonical_order_without_native_reenumeration) {
    Fixture f({"z", "alpha", "Alpine", "alpine", "al-file", "al-link"});
    f.s->observations.emplace(*NativeName::from_posix("al-file"),
                              Observation<BasicMetadata>::available(kind(ObjectKind::RegularFile)));
    f.s->observations.emplace(*NativeName::from_posix("al-link"),
                              Observation<BasicMetadata>::available(kind(ObjectKind::SymbolicLink)));
    auto source = dataset(f);
    const auto native_reads = f.s->reads;
    const auto opened = f.s->opened;
    FilesCompletionIndexJob job(source, query("/root/al"));
    CK_CHECK(!job.completed().value());
    const auto page = finish(job);
    CK_CHECK(page->complete() && page->exhausted && !page->next && page->entries.size() == 2);
    CK_CHECK(*page->entries[0].name.posix_bytes() == "alpha" && page->entries[0].row == 2 &&
             page->entries[0].text == "/root/alpha/");
    CK_CHECK(*page->entries[1].name.posix_bytes() == "alpine" && page->entries[1].row == 4);
    CK_CHECK(f.s->reads == native_reads && f.s->opened == opened);
    CK_CHECK(job.advance() == FilesCompletionIndexState::Complete);
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_row_and_byte_pages_resume_inside_read_batches_without_omissions) {
    for (int bound = 0; bound < 3; ++bound) {
        Fixture f({"e", "d", "c", "b", "a"});
        auto source = dataset(f);
        FilesCompletionIndexLimits limits;
        limits.scan.rows = 4;
        if (bound == 0)
            limits.candidates = 2;
        if (bound == 1)
            limits.text_bytes = 16; // Two /root/x/ proposals.
        if (bound == 2)
            limits.native_name_bytes = 2;
        std::optional<FilesCompletionCursor> next;
        std::string names;
        std::size_t pages = 0;
        do {
            FilesCompletionIndexJob job(source, query(), limits, next);
            auto page = finish(job);
            CK_CHECK(++pages <= 3 && page->entries.size() <= 2 && page->text_bytes <= limits.text_bytes &&
                     page->native_name_bytes <= limits.native_name_bytes);
            for (const auto &entry : page->entries)
                names += *entry.name.posix_bytes();
            next = page->next;
            CK_CHECK(page->exhausted == !next.has_value());
        } while (next);
        CK_CHECK(names == "abcde" && pages == 3);
        source.reset();
        f.released();
    }
}
CK_TEST(files_completion_index_more_requires_a_real_additional_match_and_empty_prefix_is_bounded) {
    Fixture f({"a", "b", "file", "link"});
    f.s->observations.emplace(*NativeName::from_posix("file"),
                              Observation<BasicMetadata>::available(kind(ObjectKind::RegularFile)));
    f.s->observations.emplace(*NativeName::from_posix("link"),
                              Observation<BasicMetadata>::available(kind(ObjectKind::ReparsePoint)));
    auto source = dataset(f);
    FilesCompletionIndexLimits limits;
    limits.candidates = 2;
    limits.scan.rows = 1;
    FilesCompletionIndexJob job(source, query(), limits);
    auto page = finish(job);
    CK_CHECK(page->complete() && !page->next && page->entries.size() == 2 && page->examined == 4);
    FilesCompletionIndexJob missing(source, query("/root/zz"), limits);
    auto empty = finish(missing);
    CK_CHECK(empty->complete() && empty->entries.empty());
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_unknown_kind_and_hidden_state_never_claim_complete_or_unique_matches) {
    Fixture f({"known", "unknown", "other", ".hidden"});
    f.s->observations.emplace(*NativeName::from_posix("unknown"), Observation<BasicMetadata>{});
    f.s->observations.emplace(*NativeName::from_posix("other"), Observation<BasicMetadata>::available(BasicMetadata{}));
    auto source = dataset(f);
    FilesCompletionIndexLimits limits;
    limits.show_hidden = false;
    FilesCompletionIndexJob job(source, query(), limits);
    auto page = finish(job);
    CK_CHECK(page->exhausted && !page->complete() && page->unknown_kind == 2 && page->unknown_hidden == 0 &&
             page->entries.size() == 1);
    source.reset();
    f.released();
    Fixture g({"known"});
    auto unknown_hidden = dataset(g, StorageModel::Unknown);
    FilesCompletionIndexJob hidden(unknown_hidden, query(), limits);
    page = finish(hidden);
    CK_CHECK(page->entries.size() == 1 && page->unknown_hidden == 1 && !page->complete());
    unknown_hidden.reset();
    g.released();
}
CK_TEST(files_completion_index_unrepresentable_and_per_candidate_limits_are_explicit_partial_results) {
    Fixture f({"a", "raw\xff"});
    auto source = dataset(f);
    FilesCompletionIndexJob job(source, query());
    auto page = finish(job);
    CK_CHECK(page->exhausted && !page->complete() && page->entries.size() == 1 && page->unavailable_proposals == 1 &&
             page->first_proposal_error);
    FilesCompletionIndexLimits limits;
    limits.candidate_text_bytes = 7;
    FilesCompletionIndexJob bounded(source, query(), limits);
    page = finish(bounded);
    CK_CHECK(!page->complete() && page->entries.empty() && page->unavailable_proposals == 2 &&
             page->first_proposal_error);
    FilesCompletionIndexJob exact(source, query("[\"/root\", \"", FilesLocationTextSyntax::ExactNames));
    page = finish(exact);
    CK_CHECK(page->complete() && page->entries.size() == 2);
    for (const auto &entry : page->entries) {
        auto route =
            take(parse_files_location_text(entry.text, FilesLocationTextSyntax::ExactNames, NameEncoding::PosixBytes));
        CK_CHECK(route.components().back() == entry.name);
    }
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_windows_raw_units_and_literal_names_remain_exact) {
    Fixture f({});
    f.s->names = {*NativeName::from_windows(u"literal. "),
                  *NativeName::from_windows(std::u16string(1, char16_t{0xD800}) + u"tail")};
    auto source = dataset(f, StorageModel::WindowsHandleAttributes);
    auto q = query("[\"V:\\\\root\", \"", FilesLocationTextSyntax::ExactNames, NameEncoding::WindowsUtf16);
    FilesCompletionIndexJob job(source, q);
    auto page = finish(job);
    CK_CHECK(page->complete() && page->entries.size() == 2);
    for (const auto &entry : page->entries) {
        auto route = take(
            parse_files_location_text(entry.text, FilesLocationTextSyntax::ExactNames, NameEncoding::WindowsUtf16));
        CK_CHECK(route.components().back() == entry.name);
    }
    FilesCompletionIndexJob ordinary(source,
                                     query("V:\\root\\", FilesLocationTextSyntax::Path, NameEncoding::WindowsUtf16));
    page = finish(ordinary);
    CK_CHECK(page->entries.empty() && page->unavailable_proposals == 2 && !page->complete());
    FilesCompletionIndexJob foreign(source, query());
    foreign.advance();
    CK_CHECK(foreign.state() == FilesCompletionIndexState::Failed &&
             foreign.failure()->error.operation == "completion_index.encoding");
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_continuations_reject_changed_query_dataset_scan_or_hidden_policy_before_io) {
    Fixture f({"a", "b"});
    auto source = dataset(f);
    FilesCompletionIndexLimits limits;
    limits.candidates = 1;
    FilesCompletionIndexJob first(source, query(), limits);
    auto page = finish(first);
    CK_CHECK(page->next);
    Fixture g({"a", "b"});
    auto other = dataset(g);
    for (int change = 0; change < 7; ++change) {
        auto current = limits;
        auto q = query();
        auto dataset_value = source;
        if (change == 0)
            q = query("/root/a");
        if (change == 1)
            q = query("/other/");
        if (change == 2)
            dataset_value = other;
        if (change == 3)
            current.scan.rows = 128;
        if (change == 4)
            current.scan.wire_bytes = 1024;
        if (change == 5)
            current.show_hidden = false;
        if (change == 6)
            current.candidate_text_bytes = 1024;
        const auto before = reads(f) + reads(g);
        FilesCompletionIndexJob invalid(dataset_value, q, current, page->next);
        CK_CHECK(invalid.state() == FilesCompletionIndexState::Failed && !invalid.completed().value() &&
                 reads(f) + reads(g) == before);
    }
    // Output page bounds may change without altering the deterministic scan.
    limits.candidates = 64;
    FilesCompletionIndexJob resume(source, query(), limits, page->next);
    auto last = finish(resume);
    CK_CHECK(last->entries.size() == 1 && *last->entries[0].name.posix_bytes() == "b" && last->has_previous &&
             last->exhausted && !last->complete());
    auto moved = *page->next;
    const auto retained = std::move(moved);
    (void)retained;
    const auto before = reads(f);
    FilesCompletionIndexJob invalid_move(source, query(), limits, std::move(moved));
    CK_CHECK(invalid_move.state() == FilesCompletionIndexState::Failed && reads(f) == before);
    source.reset();
    other.reset();
    f.released();
    g.released();
}
CK_TEST(files_completion_index_cancellation_preserves_shared_store_and_real_read_failure_precedence) {
    for (int phase = 0; phase < 3; ++phase) {
        Fixture f({"a", "b"});
        auto source = dataset(f);
        std::stop_source stop;
        FilesCompletionIndexJob job(source, query());
        if (phase == 0)
            stop.request_stop();
        else
            set_read_hook(f, [&] { stop.request_stop(); });
        if (phase == 2)
            for (const auto &p : f.c->files)
                if (!p->closed)
                    p->read_error =
                        UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "read_fault"}};
        job.advance(stop.get_token());
        CK_CHECK(!job.completed().value());
        if (phase == 2)
            CK_CHECK(job.state() == FilesCompletionIndexState::Failed &&
                     job.failure()->error.operation == "read_fault");
        else {
            CK_CHECK(job.state() == FilesCompletionIndexState::Cancelled &&
                     source->store()->state() == DirectoryIndexStoreState::Sealed);
            set_read_hook(f, {});
            FilesCompletionIndexJob retry(source, query());
            CK_CHECK(finish(retry)->entries.size() == 2);
        }
        source.reset();
        f.released();
    }
}
CK_TEST(files_completion_index_first_row_budget_failure_and_late_exception_expose_no_staging_prefix) {
    Fixture f({"a", "b"});
    auto source = dataset(f);
    FilesCompletionIndexLimits limits;
    limits.text_bytes = 1;
    FilesCompletionIndexJob bytes(source, query(), limits);
    bytes.advance();
    CK_CHECK(bytes.state() == FilesCompletionIndexState::Failed &&
             bytes.failure()->error.operation == "completion_index.page_bytes");
    limits = {};
    limits.scan.wire_bytes = 16;
    FilesCompletionIndexJob wire(source, query(), limits);
    wire.advance();
    CK_CHECK(wire.state() == FilesCompletionIndexState::Failed &&
             source->store()->state() == DirectoryIndexStoreState::Sealed);
    limits = {};
    limits.scan.rows = 1;
    FilesCompletionIndexJob throwing(source, query(), limits);
    throwing.advance();
    set_read_hook(f, [] { throw std::runtime_error("read exception"); });
    bool threw = false;
    try {
        throwing.advance();
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CK_CHECK(threw && throwing.state() == FilesCompletionIndexState::Failed && !throwing.completed().value());
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_pages_own_no_native_resources_and_abandonment_withdraws_results) {
    Fixture f({"a", "b"});
    auto source = dataset(f);
    FilesCompletionIndexLimits limits;
    limits.candidates = 1;
    FilesCompletionIndexJob job(source, query(), limits);
    auto page = finish(job);
    CK_CHECK(page->next && job.abandon() && !job.abandon() && !job.completed().value());
    source.reset();
    f.released();
    CK_CHECK(page->entries[0].text == "/root/a/"); // Page/cursor retain values only.
    Fixture pending({"a", "b"});
    source = dataset(pending);
    limits.scan.rows = 1;
    FilesCompletionIndexJob staging(source, query(), limits);
    staging.advance();
    CK_CHECK(staging.abandon() && !staging.completed().value());
    source.reset();
    pending.released();
    Fixture g({});
    source = dataset(g);
    FilesCompletionIndexJob empty(source, query());
    auto none = finish(empty);
    CK_CHECK(none->complete() && none->entries.empty() && !none->next);
    source.reset();
    g.released();
}
CK_TEST(files_completion_index_large_source_has_bounded_advances_pages_and_linear_continuation_reads) {
    Fixture f({});
    for (int i = 1024; i >= 0; --i)
        f.s->names.push_back(*NativeName::from_posix("d" + std::to_string(10000 + i)));
    auto source = dataset(f);
    FilesCompletionIndexLimits limits;
    limits.scan.rows = 31;
    std::optional<FilesCompletionCursor> next;
    std::size_t count = 0, pages = 0;
    std::string previous;
    const auto started_reads = reads(f);
    do {
        FilesCompletionIndexJob job(source, query("/root/d"), limits, next);
        while (job.state() == FilesCompletionIndexState::Running) {
            const auto before = reads(f);
            job.advance();
            CK_CHECK(reads(f) - before <= 2 * limits.scan.rows + 1);
        }
        const auto page = take(job.completed());
        CK_CHECK(page->entries.size() <= 64 && ++pages <= 17);
        for (const auto &entry : page->entries) {
            CK_CHECK(previous < *entry.name.posix_bytes());
            previous = *entry.name.posix_bytes();
            ++count;
        }
        next = page->next;
    } while (next);
    CK_CHECK(count == 1025 && pages == 17 && reads(f) - started_reads < 2 * (1025 + 17 * 31) + 100);
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_invalid_inputs_and_limits_do_not_read_source) {
    Fixture f;
    auto source = dataset(f);
    for (int which = 0; which < 14; ++which) {
        FilesCompletionIndexLimits limits;
        auto q = query();
        auto input = source;
        if (which == 0)
            input.reset();
        if (which == 1)
            limits.scan.rows = 0;
        if (which == 2)
            limits.scan.rows = DirectoryIndexScanLimits::hard_rows + 1;
        if (which == 3)
            limits.scan.wire_bytes = 0;
        if (which == 4)
            limits.scan.wire_bytes = DirectoryIndexScanLimits::hard_wire_bytes + 1;
        if (which == 5)
            limits.candidates = 0;
        if (which == 6)
            limits.candidates = 65;
        if (which == 7)
            limits.text_bytes = 0;
        if (which == 8)
            limits.text_bytes = 1024 * 1024 + 1;
        if (which == 9)
            limits.native_name_bytes = 0;
        if (which == 10)
            limits.candidate_text_bytes = 32769;
        if (which == 11) {
            const auto retained = std::move(q);
            (void)retained;
        }
        if (which == 12)
            limits.native_name_bytes = 1024 * 1024 + 1;
        if (which == 13)
            limits.candidate_text_bytes = 0;
        const auto before = reads(f);
        bool threw = false;
        try {
            FilesCompletionIndexJob invalid(input, q, limits);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CK_CHECK(threw && reads(f) == before);
    }
    source.reset();
    f.released();
}
CK_TEST(files_completion_index_native_private_index_matches_real_folders_without_reopening_source) {
    ck::testing::ScratchDirectory scratch{"completion-index", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT};
#ifndef _WIN32
    std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
#endif
    const auto path = scratch.path() / "source";
    std::filesystem::create_directory(path);
    std::filesystem::create_directory(path / "alpha");
    std::filesystem::create_directory(path / "alpine");
    std::ofstream(path / "al-file") << "fixture";
    NativeFileSystem fs;
    const auto address = take(NativeAbsolutePath::from_host(path));
    auto workspace = take(open_native_index_workspace(take(NativeAbsolutePath::from_host(scratch.path()))));
    auto budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 16);
    std::array<std::byte, 16> generation{};
    generation[0] = std::byte{1};
    auto listing = std::make_unique<DirectoryIndexListingJob>(
        PaneListingRequest{AdapterScope{}, 1},
        std::make_unique<DirectoryListing>(take(fs.open_native_directory(address))), StorageModel::Unknown, workspace,
        budget, generation);
    finish(*listing);
    auto source = take(listing->completed());
    listing.reset();
    auto text = take(format_files_exact_location(FilesPaneLocation(address)));
    text.pop_back();
    text += ", \"al";
    auto q = query(text, FilesLocationTextSyntax::ExactNames, address.encoding());
    FilesCompletionIndexLimits limits;
    limits.candidates = 1;
    FilesCompletionIndexJob first(source, q, limits);
    auto a = finish(first);
    CK_CHECK(a->entries.size() == 1 && a->next);
    FilesCompletionIndexJob second(source, q, limits, a->next);
    auto b = finish(second);
    CK_CHECK(b->entries.size() == 1 && b->exhausted && b->has_previous && !b->complete());
    CK_CHECK(a->entries[0].name == *NativeName::from_posix("alpha") ||
             a->entries[0].name == *NativeName::from_windows(u"alpha"));
    CK_CHECK(b->entries[0].name == *NativeName::from_posix("alpine") ||
             b->entries[0].name == *NativeName::from_windows(u"alpine"));
    source.reset();
    workspace.reset();
    CK_CHECK(budget->charged_files() == 0 && budget->charged_bytes() == 0);
}
