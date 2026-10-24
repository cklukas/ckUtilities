// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_lookup.hpp"
#include "directory_index_sort_job.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <functional>
#include <map>
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
    bool closed = false, zero_read = false, zero_write = false, over_read = false, over_write = false,
         throw_read = false, throw_write = false;
    std::size_t max_io = SIZE_MAX, reads = 0, writes = 0, sizes = 0;
    std::optional<UnavailableObservation> read_error, write_error;
    std::function<void()> on_read, on_write;
};
struct Control {
    std::vector<std::shared_ptr<File>> files;
    std::optional<UnavailableObservation> create_error, write_error;
    bool throw_create = false;
    std::function<void()> on_create;
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
        ++f->sizes;
        return Observation<std::uint64_t>::available(f->data.size());
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        ++f->writes;
        if (f->throw_write)
            throw std::runtime_error("write fault");
        if (f->zero_write)
            return Observation<std::size_t>::available(0);
        if (f->over_write)
            return Observation<std::size_t>::available(bytes.size() + 1);
        bytes = bytes.first(std::min(bytes.size(), f->max_io));
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
        ++f->reads;
        if (f->throw_read)
            throw std::runtime_error("read fault");
        if (f->zero_read)
            return Observation<std::size_t>::available(0);
        if (f->over_read)
            return Observation<std::size_t>::available(bytes.size() + 1);
        bytes = bytes.first(std::min(bytes.size(), f->max_io));
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
        if (c->on_create)
            c->on_create();
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
struct Fixture {
    DirectoryIndexContext context;
    std::shared_ptr<Control> c = std::make_shared<Control>();
    std::shared_ptr<IndexFileFactory> factory = std::make_shared<Factory>(c);
    std::shared_ptr<IndexStorageBudget> budget;
    std::shared_ptr<DirectoryIndexStore> source;
    std::map<std::uint64_t, Bytes> encoded;
    Fixture(std::uint64_t bytes = 1024 * 1024, std::size_t files = 16)
        : budget(std::make_shared<IndexStorageBudget>(bytes, files)) {
        context.generation[0] = std::byte{1};
        source = take(DirectoryIndexStore::create_new(factory, budget, context));
    }
    void add(std::uint64_t id, const char *name, ObjectKind kind, std::optional<std::uint64_t> size,
             std::optional<std::int64_t> time, bool metadata = true) {
        BasicMetadata m;
        m.kind = Observation<ObjectKind>::available(kind);
        if (size)
            m.logical_size = Observation<std::uint64_t>::available(*size);
        if (time)
            m.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(*time, 123));
        DirectoryIndexRecord r{id, *NativeName::from_posix(name),
                               metadata ? Observation<BasicMetadata>::available(std::move(m))
                                        : Observation<BasicMetadata>::unavailable(ObservationState::Denied,
                                                                                  {ErrorDomain::Posix, 13, "inspect"}),
                               Observation<bool>::available(id % 2 == 0)};
        encoded[id] = take(encode_directory_index_record(r, context));
        take(source->append(r));
    }
    void populate() {
        add(1, "z", ObjectKind::RegularFile, 5, 100);
        add(2, "b", ObjectKind::Directory, {}, 20);
        add(3, "a", ObjectKind::SymbolicLink, 10, 30);
        add(4, "c", ObjectKind::RegularFile, {}, {}, false);
        add(5, "d", ObjectKind::RegularFile, {}, {});
        add(6, "a", ObjectKind::RegularFile, 10, 30);
        add(7, "e", ObjectKind::Directory, 2, 40);
        take(source->seal());
    }
    void scratch_closed() {
        for (std::size_t i = 1; i < c->files.size(); ++i)
            CK_CHECK(c->files[i]->closed);
        CK_CHECK(budget->charged_files() == 1 && budget->charged_bytes() == source->bytes());
    }
};

void finish(DirectoryIndexLookupJob &job) {
    std::size_t steps = 0;
    while (job.state() == DirectoryIndexLookupJobState::Initializing ||
           job.state() == DirectoryIndexLookupJobState::Building) {
        if (++steps > 10000)
            throw std::runtime_error("lookup fixture step budget");
        job.advance();
    }
}
void sort_source(Fixture &f) {
    DirectoryIndexSortJob job(f.source, {PaneSortKey::Name, false, false}, {{2, 4096}, 2});
    while (job.state() == DirectoryIndexSortState::Running)
        job.advance();
    f.source = take(job.completed());
}
void source_only(Fixture &f) {
    CK_CHECK(f.budget->charged_files() == 1 && f.budget->charged_bytes() == f.source->bytes());
}
std::shared_ptr<File> live_source(Fixture &f) {
    for (auto &file : f.c->files)
        if (!file->closed)
            return file;
    throw std::runtime_error("missing source fixture");
}
void fix_crc(Bytes &b, std::size_t at) {
    auto crc = detail::directory_index_crc32c(std::span<const std::byte>(b).subspan(at, 60));
    for (unsigned i = 0; i < 4; ++i)
        b[at + 60 + i] = static_cast<std::byte>((crc >> (i * 8)) & 255);
}
} // namespace
CK_TEST(index_lookup_builds_bidirectional_positions_and_bounded_pages_without_reassigning_rows) {
    Fixture f;
    f.populate();
    sort_source(f);
    const std::vector<std::uint64_t> expected{3, 6, 2, 4, 5, 7, 1};
    DirectoryIndexLookupJob job(f.source, {2, 4096});
    finish(job);
    auto index = take(job.completed());
    CK_CHECK(index->rows() == 7 && index->bytes() == 896 && index->row_scope() == f.source->row_scope());
    CK_CHECK(f.budget->charged_files() == 2 && f.budget->charged_bytes() == f.source->bytes() + 896);
    for (std::uint64_t rank = 0; rank < expected.size(); ++rank) {
        const auto page = take(index->read_page(rank, {2, 4096}));
        CK_CHECK(page.rows.size() == std::min<std::uint64_t>(2, 7 - rank));
        CK_CHECK(page.next.position() == rank + page.rows.size());
        for (std::size_t i = 0; i < page.rows.size(); ++i) {
            const auto &row = page.rows[i];
            CK_CHECK(row.record.row_id == expected[rank + i]);
            CK_CHECK(take(encode_directory_index_record(row.record, f.context)) == f.encoded.at(row.record.row_id));
        }
        CK_CHECK(take(index->find_row({f.source->row_scope(), expected[rank]})) == rank);
    }
    const auto end = take(index->read_page(7));
    CK_CHECK(end.end && end.rows.empty() && end.next.position() == 7);
    CK_CHECK(job.abandon() && !job.completed().value());
    index.reset();
    source_only(f);
}
CK_TEST(index_lookup_empty_input_checks_native_End_without_an_extra_file_slot) {
    Fixture f(4096, 1);
    take(f.source->seal());
    DirectoryIndexLookupJob job(f.source);
    finish(job);
    auto index = take(job.completed());
    CK_CHECK(index->bytes() == 0 && f.c->files.size() == 1);
    auto page = take(index->read_page(0));
    CK_CHECK(page.end && page.rows.empty());
    CK_CHECK(!index->find_row({index->row_scope(), 1}).value());
    job.abandon();
    index.reset();
    source_only(f);
}
CK_TEST(index_lookup_reserves_complete_bytes_and_file_slot_before_factory_creation) {
    Fixture f;
    f.populate();
    const auto source_bytes = f.source->bytes();
    f.c->on_create = [&] {
        CK_CHECK(f.budget->charged_files() == 2 && f.budget->charged_bytes() == source_bytes + 896);
    };
    DirectoryIndexLookupJob job(f.source);
    CK_CHECK(f.budget->charged_bytes() == source_bytes + 896);
    job.abandon();
    source_only(f);
    for (int fault = 0; fault < 3; ++fault) {
        Fixture g(fault == 0 ? 4096 : 1024 * 1024, fault == 1 ? 1 : 16);
        g.populate();
        // Known source bytes leave less than 7*128 for the complete lookup.
        if (fault == 0) {
            auto filler = take(DirectoryIndexStore::create_new(g.factory, g.budget, g.context));
            std::uint64_t id = 1;
            while (g.budget->charged_bytes() + 896 <= 4096) {
                auto row = DirectoryIndexRecord{id++, *NativeName::from_posix("fill"), {}, {}};
                if (!filler->append(row).value())
                    break;
            }
            const auto before = g.c->files.size();
            DirectoryIndexLookupJob limited(g.source);
            CK_CHECK(limited.state() == DirectoryIndexLookupJobState::Failed && g.c->files.size() == before);
            filler.reset();
            source_only(g);
        } else {
            const auto before = g.c->files.size();
            DirectoryIndexLookupJob limited(g.source, {}, fault == 2 ? 895 : 4096);
            CK_CHECK(limited.state() == DirectoryIndexLookupJobState::Failed && g.c->files.size() == before);
            source_only(g);
        }
    }
}
CK_TEST(index_lookup_rejects_foreign_moved_and_invalid_keys_before_IO_and_keeps_valid_lookup) {
    Fixture f;
    f.populate();
    DirectoryIndexLookupJob job(f.source);
    finish(job);
    auto index = take(job.completed());
    auto file = f.c->files.back();
    const auto sizes = file->sizes, reads = file->reads;
    AdapterScope moved;
    auto retained = std::move(moved);
    (void)retained;
    for (const auto &key :
         std::vector<IndexRowKey>{{AdapterScope{}, 1}, {moved, 1}, {index->row_scope(), 0}, {index->row_scope(), 8}})
        CK_CHECK(!index->find_row(key).value());
    CK_CHECK(!index->read_page(8).value());
    bool threw = false;
    try {
        index->read_page(0, {0, 4096});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && file->sizes == sizes && file->reads == reads &&
             index->state() == DirectoryIndexLookupState::Ready);
    CK_CHECK(take(index->find_row({index->row_scope(), 1})) == 0);
}
CK_TEST(index_lookup_small_page_budget_can_retry_without_revoking_source_or_lookup) {
    Fixture f;
    f.populate();
    DirectoryIndexLookupJob job(f.source);
    finish(job);
    auto index = take(job.completed());
    CK_CHECK(!index->read_page(0, {1, 16}).value());
    CK_CHECK(index->state() == DirectoryIndexLookupState::Ready &&
             f.source->state() == DirectoryIndexStoreState::Sealed);
    CK_CHECK(take(index->read_page(0, {1, 4096})).rows.size() == 1);
}
CK_TEST(index_lookup_checks_schema_generation_roles_checksum_and_cross_references) {
    for (const auto offset : std::vector<std::size_t>{0, 8, 24, 32, 40, 48, 52, 56, 60, 7 * 64 + 24}) {
        for (bool crc_valid : {false, true}) {
            if (crc_valid && offset == 60)
                continue; // repairing the checksum alone restores the original frame
            Fixture f;
            f.populate();
            DirectoryIndexLookupJob job(f.source);
            finish(job);
            auto index = take(job.completed());
            auto &data = f.c->files.back()->data;
            data[offset] ^= std::byte{1};
            if (crc_valid)
                fix_crc(data, offset / 64 * 64);
            CK_CHECK(!index->read_page(0).value() && index->state() == DirectoryIndexLookupState::Failed);
            CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
        }
    }
}
CK_TEST(index_lookup_rejects_truncated_trailing_and_corrupted_source_without_page_prefix) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        f.populate();
        auto source = live_source(f);
        DirectoryIndexLookupJob job(f.source);
        finish(job);
        auto index = take(job.completed());
        if (fault == 0)
            f.c->files.back()->data.pop_back();
        else if (fault == 1)
            f.c->files.back()->data.push_back(std::byte{});
        else
            source->data[12] ^= std::byte{1};
        CK_CHECK(!index->read_page(0).value() && index->state() == DirectoryIndexLookupState::Failed);
        CK_CHECK(f.source->state() ==
                 (fault == 2 ? DirectoryIndexStoreState::Failed : DirectoryIndexStoreState::Sealed));
    }
}
CK_TEST(index_lookup_cancellation_at_every_build_advance_releases_only_projection) {
    Fixture baseline;
    baseline.populate();
    DirectoryIndexLookupJob complete(baseline.source, {1, 4096});
    std::size_t steps = 0;
    while (complete.state() != DirectoryIndexLookupJobState::Complete) {
        complete.advance();
        if (++steps > 100)
            throw std::runtime_error("baseline budget");
    }
    complete.abandon();
    source_only(baseline);
    for (std::size_t at = 0; at < steps; ++at) {
        Fixture f;
        f.populate();
        DirectoryIndexLookupJob job(f.source, {1, 4096});
        for (std::size_t i = 0; i < at; ++i)
            job.advance();
        std::stop_source stop;
        stop.request_stop();
        job.advance(stop.get_token());
        CK_CHECK(job.state() == DirectoryIndexLookupJobState::Cancelled && !job.completed().value());
        CK_CHECK(!job.abandon() && f.source->state() == DirectoryIndexStoreState::Sealed);
        source_only(f);
    }
    Fixture f;
    f.populate();
    const auto before = f.c->files.size();
    std::stop_source stop;
    stop.request_stop();
    DirectoryIndexLookupJob job(f.source, {}, 4096, stop.get_token());
    CK_CHECK(job.state() == DirectoryIndexLookupJobState::Cancelled && f.c->files.size() == before);
    source_only(f);
}
CK_TEST(index_lookup_cancellation_during_source_reads_does_not_poison_healthy_input) {
    for (bool ready : {false, true}) {
        Fixture f;
        f.populate();
        auto source = live_source(f);
        DirectoryIndexLookupJob job(f.source, {1, 4096});
        std::stop_source stop;
        if (ready) {
            finish(job);
            auto index = take(job.completed());
            source->on_read = [&] { stop.request_stop(); };
            CK_CHECK(!index->read_page(0, {}, stop.get_token()).value() &&
                     index->state() == DirectoryIndexLookupState::Failed);
        } else {
            while (job.state() == DirectoryIndexLookupJobState::Initializing)
                job.advance();
            source->on_read = [&] { stop.request_stop(); };
            job.advance(stop.get_token());
            CK_CHECK(job.state() == DirectoryIndexLookupJobState::Cancelled);
        }
        CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
        job.abandon();
        source_only(f);
    }
}
CK_TEST(index_lookup_partial_IO_and_invalid_progress_are_handled_exactly) {
    for (int fault = 0; fault < 5; ++fault) {
        Fixture f;
        f.populate();
        DirectoryIndexLookupJob job(f.source, {1, 4096});
        auto file = f.c->files.back();
        file->max_io = 3;
        if (fault == 1)
            file->zero_write = true;
        else if (fault == 2)
            file->over_write = true;
        else if (fault == 3)
            file->zero_read = true;
        else if (fault == 4)
            file->over_read = true;
        finish(job);
        if (fault == 0) {
            auto index = take(job.completed());
            CK_CHECK(take(index->find_row({index->row_scope(), 7})) == 6);
            CK_CHECK(take(index->read_page(6)).end);
        } else
            CK_CHECK(job.state() == DirectoryIndexLookupJobState::Failed && !job.completed().value());
        job.abandon();
        source_only(f);
    }
}
CK_TEST(index_lookup_native_failures_preserve_states_and_adapter_interruption_needs_requested_stop) {
    for (auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        for (int stage = 0; stage < 3; ++stage) {
            Fixture f;
            f.populate();
            UnavailableObservation e{state, {ErrorDomain::Posix, 28, "lookup_fault"}};
            if (stage == 0)
                f.c->create_error = e;
            DirectoryIndexLookupJob job(f.source);
            if (stage == 1)
                f.c->files.back()->write_error = e;
            if (stage == 2)
                f.c->files.back()->read_error = e;
            finish(job);
            CK_CHECK(job.state() == DirectoryIndexLookupJobState::Failed && job.failure()->state == state &&
                     job.failure()->error == e.error);
            source_only(f);
        }
    }
    Fixture f;
    f.populate();
    f.c->create_error = UnavailableObservation{
        ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "interrupted"}};
    DirectoryIndexLookupJob job(f.source);
    CK_CHECK(job.state() == DirectoryIndexLookupJobState::Failed);
    source_only(f);
}
CK_TEST(index_lookup_exceptions_revoke_and_retire_build_staging) {
    for (bool reading : {false, true}) {
        Fixture f;
        f.populate();
        DirectoryIndexLookupJob job(f.source);
        auto file = f.c->files.back();
        if (reading)
            file->throw_read = true;
        else
            file->throw_write = true;
        bool threw = false;
        try {
            finish(job);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw && job.state() == DirectoryIndexLookupJobState::Failed && !job.completed().value());
        source_only(f);
    }
    Fixture f;
    f.populate();
    f.c->throw_create = true;
    bool threw = false;
    try {
        DirectoryIndexLookupJob job(f.source);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CK_CHECK(threw);
    source_only(f);
}
CK_TEST(index_lookup_detects_duplicate_original_IDs_in_valid_checksum_sorted_source) {
    Fixture f;
    f.populate();
    sort_source(f);
    auto source = live_source(f);
    auto first = take(f.source->read_page(take(f.source->begin_scan()), {1, 4096}));
    auto row = first.rows[0].record;
    row.row_id = 6;
    auto wire = take(encode_directory_index_record(row, f.context));
    std::copy(wire.begin(), wire.end(), source->data.begin());
    DirectoryIndexLookupJob job(f.source, {1, 4096});
    finish(job);
    CK_CHECK(job.state() == DirectoryIndexLookupJobState::Failed &&
             job.failure()->error.operation == "index_lookup.duplicate_row");
    CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
    source_only(f);
}
CK_TEST(index_lookup_invalid_configuration_and_unsealed_source_do_not_create_files) {
    Fixture f;
    const auto before = f.c->files.size();
    DirectoryIndexLookupJob unsealed(f.source);
    CK_CHECK(unsealed.state() == DirectoryIndexLookupJobState::Failed && f.c->files.size() == before);
    take(f.source->seal());
    for (int fault = 0; fault < 2; ++fault) {
        bool threw = false;
        try {
            DirectoryIndexLookupJob job(f.source,
                                        fault == 0 ? DirectoryIndexScanLimits{0, 4096} : DirectoryIndexScanLimits{},
                                        fault == 1 ? 0 : 4096);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CK_CHECK(threw && f.c->files.size() == before);
    }
}

CK_TEST(index_lookup_ready_IO_failure_cancellation_and_exception_revoke_only_lookup) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        f.populate();
        DirectoryIndexLookupJob job(f.source);
        finish(job);
        auto index = take(job.completed());
        auto file = f.c->files.back();
        std::stop_source stop;
        if (fault == 0)
            file->read_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Win32, 5, "ready_read"}};
        else if (fault == 1)
            file->throw_read = true;
        else
            file->on_read = [&] { stop.request_stop(); };
        bool threw = false;
        try {
            CK_CHECK(!index->read_page(0, {}, stop.get_token()).value());
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw == (fault == 1) && index->state() == DirectoryIndexLookupState::Failed &&
                 f.source->state() == DirectoryIndexStoreState::Sealed);
        CK_CHECK(f.budget->charged_files() == 2);
        job.abandon();
        CK_CHECK(f.budget->charged_files() == 2);
        index.reset();
        source_only(f);
    }
}
CK_TEST(index_lookup_filters_positions_and_distinguishes_valid_excluded_original_keys) {
    Fixture f;
    f.populate();
    sort_source(f);
    const PaneFilter filter{false, "a", false};
    DirectoryIndexLookupJob job(f.source, {1, 4096}, 1024 * 1024, {}, filter);
    finish(job);
    auto index = take(job.completed());
    CK_CHECK(index->total_rows() == 7 && index->rows() == 1 && index->bytes() == 896);
    const auto page = take(index->read_page(0));
    CK_CHECK(page.end && page.rows.size() == 1 && page.rows[0].record.row_id == 3);
    CK_CHECK(take(index->locate_row({index->row_scope(), 3})) == 0);
    CK_CHECK(!take(index->locate_row({index->row_scope(), 6})).has_value());
    CK_CHECK(!index->find_row({index->row_scope(), 6}).value() && index->state() == DirectoryIndexLookupState::Ready);
    CK_CHECK(!index->locate_row({AdapterScope{}, 3}).value());
    CK_CHECK(!f.source->read_page(page.next).value() && f.source->state() == DirectoryIndexStoreState::Sealed);
    CK_CHECK(!index->read_page(0, {1, 16}).value() && take(index->read_page(0, {1, 4096})).rows.size() == 1);
}
CK_TEST(index_lookup_keeps_unavailable_hidden_states_visible_and_all_filtered_End_checked) {
    Fixture f;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        Observation<bool> hidden =
            i == 1   ? Observation<bool>::available(true)
            : i == 2 ? Observation<bool>::available(false)
                     : Observation<bool>::unavailable(i == 3 ? ObservationState::Unknown : ObservationState::Denied,
                                                      {ErrorDomain::Posix, 13, "hidden"});
        take(f.source->append({i, *NativeName::from_posix("item" + std::to_string(i)), {}, std::move(hidden)}));
    }
    take(f.source->seal());
    DirectoryIndexLookupJob job(f.source, {2, 4096}, 4096, {}, PaneFilter{false, "", false});
    finish(job);
    auto index = take(job.completed());
    auto page = take(index->read_page(0, {3, 4096}));
    CK_CHECK(page.end && page.rows.size() == 3 && page.rows[0].record.row_id == 2 && page.rows[2].record.row_id == 4);
    DirectoryIndexLookupJob none(f.source, {}, 4096, {}, PaneFilter{true, "absent", false});
    finish(none);
    auto empty = take(none.completed());
    CK_CHECK(empty->rows() == 0 && empty->total_rows() == 4 && take(empty->read_page(0)).end);
    for (std::uint64_t i = 1; i <= 4; ++i)
        CK_CHECK(!take(empty->locate_row({empty->row_scope(), i})).has_value());
    live_source(f)->data.push_back(std::byte{});
    CK_CHECK(!empty->read_page(0).value() && f.source->state() == DirectoryIndexStoreState::Failed);
}
CK_TEST(index_lookup_excluded_rows_have_checked_reverse_pairs_and_checksum_protection) {
    for (bool valid_crc : {false, true}) {
        Fixture f;
        f.populate();
        DirectoryIndexLookupJob job(f.source, {}, 4096, {}, PaneFilter{true, "absent", false});
        finish(job);
        auto index = take(job.completed());
        auto &data = f.c->files.back()->data;
        // Original ID1 points to excluded tail position6. Alter its counterpart.
        data[6 * 64 + 40] ^= std::byte{1};
        if (valid_crc)
            fix_crc(data, 6 * 64);
        CK_CHECK(!index->locate_row({index->row_scope(), 1}).value() &&
                 index->state() == DirectoryIndexLookupState::Failed);
        CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
    }
}
CK_TEST(index_lookup_filtered_build_cancellation_preserves_source_and_releases_every_reservation) {
    Fixture f;
    f.populate();
    DirectoryIndexLookupJob baseline(f.source, {1, 4096}, 4096, {}, PaneFilter{false, "a", false});
    std::size_t steps = 0;
    while (baseline.state() != DirectoryIndexLookupJobState::Complete) {
        baseline.advance();
        if (++steps > 100)
            throw std::runtime_error("filter baseline");
    }
    baseline.abandon();
    for (std::size_t at = 0; at < steps; ++at) {
        DirectoryIndexLookupJob job(f.source, {1, 4096}, 4096, {}, PaneFilter{false, "a", false});
        for (std::size_t i = 0; i < at; ++i)
            job.advance();
        std::stop_source stop;
        stop.request_stop();
        job.advance(stop.get_token());
        CK_CHECK(job.state() == DirectoryIndexLookupJobState::Cancelled);
        source_only(f);
    }
}
