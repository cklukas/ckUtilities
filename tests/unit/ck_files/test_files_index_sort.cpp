// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
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
void finish(DirectoryIndexSortJob &job) {
    std::size_t steps = 0;
    while (job.state() == DirectoryIndexSortState::Running && steps++ < 10000)
        job.advance();
    CK_CHECK(job.state() == DirectoryIndexSortState::Complete);
    if (job.state() != DirectoryIndexSortState::Complete)
        throw std::runtime_error("sort not complete");
}
std::vector<std::uint64_t> ids(DirectoryIndexStore &store, Fixture &f) {
    auto cursor = take(store.begin_scan());
    std::vector<std::uint64_t> result;
    for (;;) {
        auto page = take(store.read_page(cursor, {2, 4096}));
        for (const auto &row : page.rows) {
            result.push_back(row.record.row_id);
            CK_CHECK(take(encode_directory_index_record(row.record, f.context)) == f.encoded.at(row.record.row_id));
            CK_CHECK(take(store.read(row.address)).row_id == row.record.row_id);
        }
        cursor = page.next;
        if (page.end)
            break;
    }
    return result;
}
} // namespace
CK_TEST(index_sort_external_merges_preserve_every_record_and_share_pane_order) {
    struct Case {
        PaneOrder order;
        std::vector<std::uint64_t> expected;
    };
    const Case cases[] = {{{PaneSortKey::Name, false, false}, {3, 6, 2, 4, 5, 7, 1}},
                          {{PaneSortKey::Name, true, false}, {1, 7, 5, 4, 2, 3, 6}},
                          {{PaneSortKey::Name, false, true}, {2, 7, 3, 6, 4, 5, 1}},
                          {{PaneSortKey::Name, true, true}, {7, 2, 1, 5, 4, 3, 6}},
                          {{PaneSortKey::Size, false, false}, {7, 1, 3, 6, 2, 4, 5}},
                          {{PaneSortKey::Size, true, false}, {3, 6, 1, 7, 2, 4, 5}},
                          {{PaneSortKey::Size, false, true}, {7, 2, 1, 3, 6, 4, 5}},
                          {{PaneSortKey::Size, true, true}, {7, 2, 3, 6, 1, 4, 5}},
                          {{PaneSortKey::Modified, false, false}, {2, 3, 6, 7, 1, 4, 5}},
                          {{PaneSortKey::Modified, true, false}, {1, 7, 3, 6, 2, 4, 5}},
                          {{PaneSortKey::Modified, false, true}, {2, 7, 3, 6, 1, 4, 5}},
                          {{PaneSortKey::Modified, true, true}, {7, 2, 1, 3, 6, 4, 5}},
                          {{PaneSortKey::Kind, false, false}, {6, 5, 1, 2, 7, 3, 4}},
                          {{PaneSortKey::Kind, true, false}, {3, 2, 7, 6, 5, 1, 4}},
                          {{PaneSortKey::Kind, false, true}, {2, 7, 6, 5, 1, 3, 4}},
                          {{PaneSortKey::Kind, true, true}, {2, 7, 3, 6, 5, 1, 4}}};
    for (const auto &test : cases) {
        Fixture f;
        f.populate();
        const auto source_bytes = f.c->files.front()->data;
        DirectoryIndexSortJob job(f.source, test.order, {{2, 4096}, 1});
        CK_CHECK(!job.completed().value());
        finish(job);
        auto output = take(job.completed());
        CK_CHECK(ids(*output, f) == test.expected);
        CK_CHECK(output->row_scope() == f.source->row_scope());
        CK_CHECK(f.c->files.front()->data == source_bytes && f.source->state() == DirectoryIndexStoreState::Sealed);
        output.reset();
        CK_CHECK(job.abandon());
        CK_CHECK(!job.abandon());
        f.scratch_closed();
    }
}
CK_TEST(index_sort_can_resort_projection_without_reassigning_original_IDs) {
    Fixture f;
    f.populate();
    DirectoryIndexSortJob first(f.source, {PaneSortKey::Name, true, false}, {{2, 4096}, 2});
    finish(first);
    auto view = take(first.completed());
    DirectoryIndexSortJob second(view, {PaneSortKey::Size, false, false}, {{3, 4096}, 1});
    finish(second);
    auto result = take(second.completed());
    CK_CHECK(ids(*result, f) == std::vector<std::uint64_t>({7, 1, 3, 6, 2, 4, 5}));
    CK_CHECK(result->row_scope() == f.source->row_scope());
    result.reset();
    second.abandon();
    view.reset();
    first.abandon();
    f.scratch_closed();
}
CK_TEST(index_sort_empty_input_validates_End_without_creating_scratch_files) {
    Fixture f(1024, 1);
    take(f.source->seal());
    DirectoryIndexSortJob job(f.source, {});
    finish(job);
    CK_CHECK(take(job.completed()) == f.source && f.c->files.size() == 1);
    job.abandon();
    f.scratch_closed();
}
CK_TEST(index_sort_invalid_configuration_and_unsealed_input_do_no_IO) {
    Fixture f;
    for (auto limits : {DirectoryIndexSortLimits{{0, 4096}, 1}, DirectoryIndexSortLimits{{2, 15}, 1},
                        DirectoryIndexSortLimits{{2, 4096}, 0}}) {
        bool threw = false;
        try {
            DirectoryIndexSortJob job(f.source, {}, limits);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CK_CHECK(threw);
    }
    DirectoryIndexSortJob unsealed(f.source, {});
    CK_CHECK(unsealed.state() == DirectoryIndexSortState::Failed);
    bool threw = false;
    try {
        DirectoryIndexSortJob invalid(f.source, {static_cast<PaneSortKey>(99), false, false});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && f.c->files.size() == 1);
}
CK_TEST(index_sort_cancellation_at_each_advance_keeps_source_usable_and_releases_runs) {
    std::size_t advances = 0;
    {
        Fixture f;
        f.populate();
        DirectoryIndexSortJob baseline(f.source, {}, {{2, 4096}, 1});
        while (baseline.state() == DirectoryIndexSortState::Running) {
            baseline.advance();
            ++advances;
        }
    }
    for (std::size_t target = 0; target < advances; ++target) {
        Fixture f;
        f.populate();
        DirectoryIndexSortJob job(f.source, {}, {{2, 4096}, 1});
        for (std::size_t i = 0; i < target; ++i)
            job.advance();
        std::stop_source stop;
        stop.request_stop();
        CK_CHECK(job.advance(stop.get_token()) == DirectoryIndexSortState::Cancelled);
        CK_CHECK(!job.completed().value());
        CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
        f.scratch_closed();
    }
}
CK_TEST(index_sort_stop_during_source_page_preserves_valid_source) {
    Fixture f;
    f.populate();
    std::stop_source stop;
    f.c->files.front()->on_read = [&] { stop.request_stop(); };
    DirectoryIndexSortJob job(f.source, {}, {{2, 4096}, 1});
    CK_CHECK(job.advance(stop.get_token()) == DirectoryIndexSortState::Cancelled);
    CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
    f.c->files.front()->on_read = {};
    CK_CHECK(take(f.source->read_page(take(f.source->begin_scan()))).rows.size() == 7);
    f.scratch_closed();
}
CK_TEST(index_sort_create_and_write_failures_preserve_native_states_without_prefix) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed})
        for (bool writing : {false, true}) {
            Fixture f;
            f.populate();
            UnavailableObservation error{state, {ErrorDomain::Win32, 112, "injected"}};
            if (writing)
                f.c->write_error = error;
            else
                f.c->create_error = error;
            DirectoryIndexSortJob job(f.source, {}, {{2, 4096}, 1});
            job.advance();
            CK_CHECK(job.state() == DirectoryIndexSortState::Failed);
            CK_CHECK(job.failure()->state == state && job.failure()->error == error.error && !job.completed().value());
            CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
            f.scratch_closed();
        }
}
CK_TEST(index_sort_quota_and_page_exhaustion_never_fall_back_to_RAM) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f(mode == 1 ? 1800 : 1024 * 1024, mode == 0 ? 1 : 16);
        f.populate();
        DirectoryIndexSortJob job(f.source, {}, {{2, mode == 2 ? 16U : 4096U}, 1});
        std::size_t steps = 0;
        while (job.state() == DirectoryIndexSortState::Running && steps++ < 100)
            job.advance();
        CK_CHECK(job.state() == DirectoryIndexSortState::Failed && !job.completed().value());
        CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
        f.scratch_closed();
    }
}
CK_TEST(index_sort_source_corruption_and_factory_exception_revoke_no_partial_projection) {
    Fixture f;
    f.populate();
    f.c->files.front()->data.back() ^= std::byte{1};
    DirectoryIndexSortJob corrupt(f.source, {}, {{2, 4096}, 1});
    while (corrupt.state() == DirectoryIndexSortState::Running)
        corrupt.advance();
    CK_CHECK(corrupt.state() == DirectoryIndexSortState::Failed &&
             f.source->state() == DirectoryIndexStoreState::Failed);
    f.scratch_closed();
    Fixture g;
    g.populate();
    g.c->throw_create = true;
    DirectoryIndexSortJob throwing(g.source, {});
    bool threw = false;
    try {
        throwing.advance();
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CK_CHECK(threw && throwing.state() == DirectoryIndexSortState::Failed && !throwing.failure());
    CK_CHECK(g.source->state() == DirectoryIndexStoreState::Sealed);
    g.scratch_closed();
}
CK_TEST(index_sort_merge_read_faults_and_write_exceptions_release_both_inputs_safely) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        f.populate();
        DirectoryIndexSortJob job(f.source, {}, {{2, 4096}, 1});
        while (f.c->files.size() < 4 && job.state() == DirectoryIndexSortState::Running)
            job.advance();
        CK_CHECK(f.c->files.size() == 4);
        if (mode == 0)
            f.c->files[2]->read_error =
                UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "merge.right"}};
        if (mode == 1)
            f.c->files[1]->data.back() ^= std::byte{1};
        if (mode == 2)
            f.c->files[3]->on_write = [] { throw std::runtime_error("merge write exception"); };
        bool threw = false;
        try {
            while (job.state() == DirectoryIndexSortState::Running)
                job.advance();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(job.state() == DirectoryIndexSortState::Failed && !job.completed().value());
        CK_CHECK(threw == (mode == 2));
        CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
        f.scratch_closed();
    }
}
CK_TEST(index_sort_merge_cancellation_and_foreign_cache_addresses_do_not_change_row_namespace) {
    Fixture f;
    f.populate();
    const auto source_address = take(f.source->read_page(take(f.source->begin_scan()))).rows.front().address;
    DirectoryIndexSortJob complete(f.source, {}, {{2, 4096}, 1});
    finish(complete);
    auto result = take(complete.completed());
    CK_CHECK(!result->read(source_address).value() && result->state() == DirectoryIndexStoreState::Sealed);
    result.reset();
    complete.abandon();
    DirectoryIndexSortJob cancel(f.source, {}, {{2, 4096}, 1});
    const auto prior = f.c->files.size();
    while (f.c->files.size() < prior + 3)
        cancel.advance();
    std::stop_source stop;
    f.c->files.back()->on_write = [&] { stop.request_stop(); };
    cancel.advance(stop.get_token());
    CK_CHECK(cancel.state() == DirectoryIndexSortState::Cancelled);
    CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
    f.scratch_closed();
}
CK_TEST(index_sort_adapter_interruption_without_requested_stop_is_a_failure) {
    Fixture f;
    f.populate();
    f.c->create_error = UnavailableObservation{
        ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "provider.interrupted"}};
    DirectoryIndexSortJob job(f.source, {});
    job.advance();
    CK_CHECK(job.state() == DirectoryIndexSortState::Failed && job.failure()->error == f.c->create_error->error);
    CK_CHECK(f.source->state() == DirectoryIndexStoreState::Sealed);
    f.scratch_closed();
}
