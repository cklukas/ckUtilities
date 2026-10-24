// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_store.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <type_traits>

namespace {
using namespace ck::files;
static_assert(!std::is_copy_constructible_v<IndexStorageBudget>);
static_assert(!std::is_move_constructible_v<IndexStorageBudget>);
template <class T> T take(Observation<T> result) {
    if (!result.value())
        throw std::runtime_error("index store fixture: " + result.unavailable_detail()->error.operation);
    return std::move(result).take_value();
}
DirectoryIndexContext context() {
    DirectoryIndexContext c;
    c.generation[0] = std::byte{1};
    return c;
}
DirectoryIndexRecord row(std::uint64_t id = 1) {
    return {id, *NativeName::from_posix("file"), {}, Observation<bool>::available(false)};
}
struct Control {
    std::vector<std::byte> data;
    std::size_t chunk = 7, reads = 0, writes = 0, sizes = 0, closed = 0;
    bool zero_write = false, zero_read = false, oversized = false, throw_io = false;
    std::optional<UnavailableObservation> write_error, read_error, size_error;
    std::function<void()> on_write, on_read, on_size, on_close;
};
class FakeFile final : public IndexByteFile {
  public:
    explicit FakeFile(std::shared_ptr<Control> control) : c(std::move(control)) {
    }
    ~FakeFile() override {
        if (c->on_close)
            c->on_close();
        ++c->closed;
    }
    Observation<std::uint64_t> size() override {
        ++c->sizes;
        if (c->on_size)
            c->on_size();
        if (c->size_error)
            return Observation<std::uint64_t>::unavailable(c->size_error->state, c->size_error->error);
        return Observation<std::uint64_t>::available(c->data.size());
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        ++c->writes;
        if (c->throw_io)
            throw std::runtime_error("injected write exception");
        if (c->write_error)
            return Observation<std::size_t>::unavailable(c->write_error->state, c->write_error->error);
        if (c->zero_write)
            return Observation<std::size_t>::available(0);
        if (c->oversized)
            return Observation<std::size_t>::available(bytes.size() + 1);
        const auto n = std::min(bytes.size(), c->chunk);
        const auto at = static_cast<std::size_t>(offset);
        c->data.resize(at + n);
        std::copy_n(bytes.begin(), n, c->data.begin() + static_cast<std::ptrdiff_t>(at));
        if (c->on_write)
            c->on_write();
        return Observation<std::size_t>::available(n);
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) override {
        ++c->reads;
        if (c->throw_io)
            throw std::runtime_error("injected read exception");
        if (c->read_error)
            return Observation<std::size_t>::unavailable(c->read_error->state, c->read_error->error);
        if (c->zero_read)
            return Observation<std::size_t>::available(0);
        if (c->oversized)
            return Observation<std::size_t>::available(bytes.size() + 1);
        const auto at = static_cast<std::size_t>(offset);
        const auto n = std::min({bytes.size(), c->chunk, at <= c->data.size() ? c->data.size() - at : std::size_t{0}});
        std::copy_n(c->data.begin() + static_cast<std::ptrdiff_t>(std::min(at, c->data.size())), n, bytes.begin());
        if (c->on_read)
            c->on_read();
        return Observation<std::size_t>::available(n);
    }

  private:
    std::shared_ptr<Control> c;
};
struct FactoryControl {
    std::shared_ptr<Control> file = std::make_shared<Control>();
    std::size_t calls = 0, closed = 0;
    bool null_file = false;
    std::optional<UnavailableObservation> error;
    std::function<void()> on_create, on_created, on_close;
};
class FakeFactory final : public IndexFileFactory {
  public:
    explicit FakeFactory(std::shared_ptr<FactoryControl> control) : c(std::move(control)) {
    }
    ~FakeFactory() override {
        if (c->on_close)
            c->on_close();
        ++c->closed;
    }
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        ++c->calls;
        if (c->on_create)
            c->on_create();
        if (c->error)
            return Observation<std::unique_ptr<IndexByteFile>>::unavailable(c->error->state, c->error->error);
        std::unique_ptr<IndexByteFile> file;
        if (!c->null_file)
            file = std::make_unique<FakeFile>(c->file);
        if (c->on_created)
            c->on_created();
        return Observation<std::unique_ptr<IndexByteFile>>::available(std::move(file));
    }

  private:
    std::shared_ptr<FactoryControl> c;
};
std::shared_ptr<IndexFileFactory> factory_for(std::shared_ptr<Control> file) {
    auto c = std::make_shared<FactoryControl>();
    c->file = std::move(file);
    return std::make_shared<FakeFactory>(std::move(c));
}
struct Fixture {
    DirectoryIndexContext ctx = context();
    std::shared_ptr<Control> c = std::make_shared<Control>();
    std::shared_ptr<IndexStorageBudget> budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 4);
    std::unique_ptr<DirectoryIndexStore> make(DirectoryIndexStoreLimits limits = {}) {
        return take(DirectoryIndexStore::create_new(factory_for(c), budget, ctx, limits));
    }
};
} // namespace
CK_TEST(index_store_seals_exact_records_and_reads_them_through_repeated_short_IO) {
    Fixture f;
    auto store = f.make();
    const auto first = take(store->append(row()));
    const auto second = take(store->append(row(2)));
    CK_CHECK(!store->read(first).value());
    CK_CHECK((f.c->reads) == (std::size_t{0}));
    CK_CHECK((store->rows()) == (std::uint64_t{2}));
    CK_CHECK((store->bytes()) == (f.c->data.size()));
    CK_CHECK((store->bytes()) == (f.budget->charged_bytes()));
    CK_CHECK(take(store->seal()));
    CK_CHECK(take(store->seal()));
    CK_CHECK((take(store->read(second)).row_id) == (std::uint64_t{2}));
    CK_CHECK((take(store->read(first)).name) == (row().name));
    CK_CHECK(f.c->reads > 2 && f.c->writes > 2);
    CK_CHECK(!store->append(row(3)).value());
    store.reset();
    CK_CHECK((f.c->closed) == (std::size_t{1}));
    CK_CHECK((f.budget->charged_bytes()) == (std::uint64_t{0}));
    CK_CHECK((f.budget->charged_files()) == (std::size_t{0}));
}
CK_TEST(index_store_shared_budget_rejects_exhaustion_without_changing_earlier_records) {
    Fixture f;
    const auto size = take(encode_directory_index_record(row(), f.ctx)).size();
    f.budget = std::make_shared<IndexStorageBudget>(size, 2);
    auto a = f.make();
    auto b_control = std::make_shared<Control>();
    auto b = take(DirectoryIndexStore::create_new(factory_for(b_control), f.budget, f.ctx));
    const auto address = take(a->append(row()));
    CK_CHECK((b->append(row()).state()) == (ObservationState::Unknown));
    CK_CHECK((b_control->writes) == (std::size_t{0}));
    CK_CHECK((b->state()) == (DirectoryIndexStoreState::Writing));
    CK_CHECK(take(a->seal()));
    CK_CHECK(take(a->read(address)).row_id == 1);
    a.reset();
    CK_CHECK(b->append(row()).value());
}
CK_TEST(index_store_enforces_exact_independent_row_and_byte_limits) {
    Fixture f;
    const auto size = take(encode_directory_index_record(row(), f.ctx)).size();
    auto store = f.make({1, size, {}});
    CK_CHECK(store->append(row()).value());
    const auto writes = f.c->writes;
    CK_CHECK((store->append(row(2)).state()) == (ObservationState::Unknown));
    CK_CHECK((f.c->writes) == (writes));
    CK_CHECK(take(store->seal()));
    Fixture other;
    auto small = other.make({2, size - 1, {}});
    CK_CHECK((small->append(row()).state()) == (ObservationState::Unknown));
    CK_CHECK((other.c->writes) == (std::size_t{0}));
    CK_CHECK((other.budget->charged_bytes()) == (std::uint64_t{0}));
}
CK_TEST(index_store_requires_empty_backend_valid_context_limits_and_bounded_file_count) {
    Fixture f;
    f.c->data.push_back(std::byte{1});
    CK_CHECK(!DirectoryIndexStore::create_new(factory_for(f.c), f.budget, f.ctx).value());
    CK_CHECK((f.c->closed) == (std::size_t{1}));
    f.c->data.clear();
    auto invalid = f.ctx;
    invalid.generation.fill(std::byte{});
    CK_CHECK(!DirectoryIndexStore::create_new(factory_for(f.c), f.budget, invalid).value());
    CK_CHECK(!DirectoryIndexStore::create_new({}, f.budget, f.ctx).value());
    CK_CHECK(!DirectoryIndexStore::create_new(factory_for(f.c), {}, f.ctx).value());
    const DirectoryIndexStoreLimits bad[] = {{0, 10, {}},         {1, 0, {}},          {1, UINT64_MAX, {}},
                                             {1, 10, {15, 1, 0}}, {1, 10, {16, 0, 0}}, {1, 10, {16, 1, 300000}}};
    for (const auto limits : bad)
        CK_CHECK(!DirectoryIndexStore::create_new(factory_for(f.c), f.budget, f.ctx, limits).value());
    CK_CHECK((f.budget->charged_files()) == (std::size_t{0}));
    f.budget = std::make_shared<IndexStorageBudget>(1024, 1);
    auto store = f.make();
    auto c2 = std::make_shared<Control>();
    CK_CHECK((DirectoryIndexStore::create_new(factory_for(c2), f.budget, f.ctx).state()) ==
             (ObservationState::Unknown));
    CK_CHECK((c2->sizes) == (std::size_t{0}));
    CK_CHECK((c2->closed) == (std::size_t{0}));
    for (const auto bytes : {std::uint64_t{0}, UINT64_MAX}) {
        bool threw = false;
        try {
            IndexStorageBudget budget(bytes, 1);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CK_CHECK(threw);
    }
}
CK_TEST(index_store_rejects_duplicate_skipped_and_invalid_records_without_writes) {
    Fixture f;
    auto store = f.make();
    CK_CHECK(!store->append(row(0)).value());
    CK_CHECK(!store->append(row(2)).value());
    auto invalid = row();
    BasicMetadata metadata;
    metadata.identity =
        Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::PosixDeviceInode, {}, {}, {}});
    invalid.metadata = Observation<BasicMetadata>::available(metadata);
    CK_CHECK(!store->append(invalid).value());
    CK_CHECK((f.c->writes) == (std::size_t{0}));
    CK_CHECK(store->append(row()).value());
    CK_CHECK(!store->append(row()).value());
    CK_CHECK(store->append(row(2)).value());
}
CK_TEST(index_store_address_cannot_cross_stores_even_for_identical_wire_contexts) {
    Fixture f;
    auto a = f.make();
    const auto address = take(a->append(row()));
    CK_CHECK(take(a->seal()));
    auto b_control = std::make_shared<Control>();
    auto b = take(DirectoryIndexStore::create_new(factory_for(b_control), f.budget, f.ctx));
    CK_CHECK(b->append(row()).value());
    CK_CHECK(take(b->seal()));
    CK_CHECK(!b->read(address).value());
    CK_CHECK((b_control->reads) == (std::size_t{0}));
    CK_CHECK((b->state()) == (DirectoryIndexStoreState::Sealed));
}
CK_TEST(index_store_preserves_every_native_failure_state_on_write_and_retains_full_reservation) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        Fixture f;
        auto store = f.make();
        f.c->write_error = UnavailableObservation{state, {ErrorDomain::Win32, 112, "native.write"}};
        auto result = store->append(row());
        CK_CHECK((result.state()) == (state));
        CK_CHECK((result.unavailable_detail()->error) == (f.c->write_error->error));
        CK_CHECK((store->state()) == (DirectoryIndexStoreState::Failed));
        CK_CHECK((store->rows()) == (std::uint64_t{0}));
        CK_CHECK((store->bytes()) == (std::uint64_t{0}));
        CK_CHECK((f.budget->charged_bytes()) == (take(encode_directory_index_record(row(), f.ctx)).size()));
        CK_CHECK(!store->seal().value());
        CK_CHECK(!store->append(row()).value());
        CK_CHECK((f.c->writes) == (std::size_t{1}));
        const auto charge = f.budget->charged_bytes();
        f.c->on_close = [&] { CK_CHECK((f.budget->charged_bytes()) == (charge)); };
        store.reset();
        CK_CHECK((f.budget->charged_bytes()) == (std::uint64_t{0}));
        f.c->on_close = {};
    }
}
CK_TEST(index_store_zero_or_excessive_IO_counts_fail_without_retry_loops) {
    for (bool excessive : {false, true}) {
        Fixture f;
        auto store = f.make();
        f.c->zero_write = !excessive;
        f.c->oversized = excessive;
        CK_CHECK(!store->append(row()).value());
        CK_CHECK((store->state()) == (DirectoryIndexStoreState::Failed));
        CK_CHECK((f.c->writes) == (std::size_t{1}));
        Fixture g;
        auto reading = g.make();
        const auto address = take(reading->append(row()));
        CK_CHECK(take(reading->seal()));
        g.c->zero_read = !excessive;
        g.c->oversized = excessive;
        CK_CHECK(!reading->read(address).value());
        CK_CHECK((reading->state()) == (DirectoryIndexStoreState::Failed));
        CK_CHECK((g.c->reads) == (std::size_t{1}));
    }
}
CK_TEST(index_store_corrupt_truncated_or_substituted_records_revoke_sealed_results) {
    for (int mode = 0; mode != 3; ++mode) {
        Fixture f;
        auto store = f.make();
        const auto address = take(store->append(row()));
        CK_CHECK(take(store->seal()));
        if (mode == 0)
            f.c->data.back() ^= std::byte{1};
        if (mode == 1)
            f.c->data.resize(f.c->data.size() - 1);
        if (mode == 2)
            f.c->data = take(encode_directory_index_record(row(2), f.ctx));
        CK_CHECK(!store->read(address).value());
        CK_CHECK((store->state()) == (DirectoryIndexStoreState::Failed));
        const auto reads = f.c->reads;
        CK_CHECK(!store->read(address).value());
        CK_CHECK((f.c->reads) == (reads));
    }
}
CK_TEST(index_store_read_and_size_errors_remain_typed_and_no_prefix_is_published) {
    Fixture f;
    f.c->size_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "size"}};
    auto result = DirectoryIndexStore::create_new(factory_for(f.c), f.budget, f.ctx);
    CK_CHECK((result.state()) == (ObservationState::Denied));
    CK_CHECK((f.budget->charged_files()) == (std::size_t{0}));
    f.c->size_error.reset();
    auto store = f.make();
    const auto address = take(store->append(row()));
    f.c->data.push_back(std::byte{0});
    CK_CHECK(!store->seal().value());
    CK_CHECK((store->state()) == (DirectoryIndexStoreState::Failed));
    Fixture g;
    auto reader = g.make();
    const auto other = take(reader->append(row()));
    CK_CHECK(take(reader->seal()));
    g.c->read_error = UnavailableObservation{ObservationState::Gone, {ErrorDomain::Posix, 5, "read"}};
    CK_CHECK((reader->read(other).state()) == (ObservationState::Gone));
    CK_CHECK((reader->failure()->error) == (g.c->read_error->error));
    CK_CHECK(!store->read(address).value());
}
CK_TEST(index_store_cancellation_during_partial_write_retains_bytes_and_revokes_publication) {
    Fixture f;
    auto store = f.make();
    std::stop_source stop;
    f.c->on_write = [&] { stop.request_stop(); };
    CK_CHECK(!store->append(row(), stop.get_token()).value());
    CK_CHECK((f.c->data.size()) == (f.c->chunk));
    CK_CHECK((f.c->writes) == (std::size_t{1}));
    CK_CHECK((store->state()) == (DirectoryIndexStoreState::Failed));
    CK_CHECK(f.budget->charged_bytes() > f.c->data.size());
    CK_CHECK(!store->seal().value());
    Fixture g;
    auto reader = g.make();
    const auto address = take(reader->append(row()));
    CK_CHECK(take(reader->seal()));
    CK_CHECK(!reader->read(address, stop.get_token()).value());
    CK_CHECK((g.c->reads) == (std::size_t{0}));
    CK_CHECK((reader->state()) == (DirectoryIndexStoreState::Failed));
}
CK_TEST(index_store_backend_exceptions_revoke_state_and_keep_charges_until_destruction) {
    for (bool reading : {false, true}) {
        Fixture f;
        auto store = f.make();
        const auto address = take(store->append(row()));
        if (reading)
            CK_CHECK(take(store->seal()));
        f.c->throw_io = true;
        bool threw = false;
        try {
            if (reading)
                (void)store->read(address);
            else
                (void)store->append(row(2));
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw);
        CK_CHECK((store->state()) == (DirectoryIndexStoreState::Failed));
        CK_CHECK(!store->failure());
        CK_CHECK(f.budget->charged_bytes() >= store->bytes());
        CK_CHECK(!store->seal().value());
        store.reset();
        CK_CHECK((f.budget->charged_bytes()) == (std::uint64_t{0}));
    }
}
CK_TEST(index_store_seals_empty_index_without_data_reads_or_writes) {
    Fixture f;
    auto store = f.make();
    CK_CHECK(take(store->seal()));
    CK_CHECK((store->rows()) == (std::uint64_t{0}));
    CK_CHECK((store->bytes()) == (std::uint64_t{0}));
    CK_CHECK((f.c->reads + f.c->writes) == (std::size_t{0}));
}
CK_TEST(index_store_partial_read_failures_preserve_every_native_state_without_publishing_prefix) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        Fixture f;
        auto store = f.make();
        const auto address = take(store->append(row()));
        CK_CHECK(take(store->seal()));
        f.c->on_read = [&] { f.c->read_error = UnavailableObservation{state, {ErrorDomain::Posix, 5, "read"}}; };
        const auto result = store->read(address);
        CK_CHECK(result.state() == state);
        CK_CHECK(result.unavailable_detail()->error == f.c->read_error->error);
        CK_CHECK(f.c->reads == 2);
        CK_CHECK(store->state() == DirectoryIndexStoreState::Failed);
    }
}
CK_TEST(index_store_partial_write_native_failure_and_partial_read_cancellation_are_terminal) {
    Fixture f;
    auto store = f.make();
    f.c->on_write = [&] {
        f.c->write_error = UnavailableObservation{ObservationState::Failed, {ErrorDomain::Win32, 112, "write"}};
    };
    CK_CHECK(store->append(row()).state() == ObservationState::Failed);
    CK_CHECK(f.c->data.size() == f.c->chunk);
    CK_CHECK(f.c->writes == 2);
    CK_CHECK(f.budget->charged_bytes() > f.c->data.size());
    Fixture g;
    auto reader = g.make();
    const auto address = take(reader->append(row()));
    CK_CHECK(take(reader->seal()));
    std::stop_source stop;
    g.c->on_read = [&] { stop.request_stop(); };
    CK_CHECK(!reader->read(address, stop.get_token()).value());
    CK_CHECK(g.c->reads == 1);
    CK_CHECK(reader->state() == DirectoryIndexStoreState::Failed);
}
CK_TEST(index_store_seal_size_failure_and_early_cancellation_never_publish) {
    Fixture f;
    auto store = f.make();
    CK_CHECK(store->append(row()).value());
    f.c->size_error = UnavailableObservation{ObservationState::Gone, {ErrorDomain::Posix, 5, "size"}};
    CK_CHECK(store->seal().state() == ObservationState::Gone);
    CK_CHECK(store->state() == DirectoryIndexStoreState::Failed);
    std::stop_source stop;
    stop.request_stop();
    Fixture g;
    auto cancelled = g.make();
    CK_CHECK(!cancelled->append(row(), stop.get_token()).value());
    CK_CHECK(g.c->writes == 0);
    CK_CHECK(g.budget->charged_bytes() == 0);
    Fixture h;
    auto sealing = h.make();
    CK_CHECK(!sealing->seal(stop.get_token()).value());
    CK_CHECK(sealing->state() == DirectoryIndexStoreState::Failed);
}
CK_TEST(index_store_stop_requested_by_final_IO_is_checked_before_publication) {
    Fixture f;
    auto writer = f.make();
    f.c->chunk = 1024;
    std::stop_source write_stop;
    f.c->on_write = [&] { write_stop.request_stop(); };
    CK_CHECK(!writer->append(row(), write_stop.get_token()).value());
    CK_CHECK(f.c->writes == 1);
    CK_CHECK(writer->rows() == 0);
    CK_CHECK(writer->state() == DirectoryIndexStoreState::Failed);
    Fixture g;
    auto reader = g.make();
    const auto address = take(reader->append(row()));
    CK_CHECK(take(reader->seal()));
    g.c->chunk = 1024;
    std::stop_source read_stop;
    g.c->on_read = [&] { read_stop.request_stop(); };
    CK_CHECK(!reader->read(address, read_stop.get_token()).value());
    CK_CHECK(g.c->reads == 1);
    Fixture h;
    auto sealing = h.make();
    std::stop_source seal_stop;
    h.c->on_size = [&] { seal_stop.request_stop(); };
    CK_CHECK(!sealing->seal(seal_stop.get_token()).value());
    CK_CHECK(sealing->state() == DirectoryIndexStoreState::Failed);
}
CK_TEST(index_store_factory_slot_is_reserved_before_creation_and_empty_size_query) {
    auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
    auto c = std::make_shared<FactoryControl>();
    auto factory = std::make_shared<FakeFactory>(c);
    c->on_create = [&] { CK_CHECK(budget->charged_files() == 1 && budget->charged_bytes() == 0); };
    c->file->on_size = c->on_create;
    auto store = take(DirectoryIndexStore::create_new(factory, budget, context()));
    CK_CHECK(c->calls == 1 && budget->charged_files() == 1);
    CK_CHECK(DirectoryIndexStore::create_new(factory, budget, context()).state() == ObservationState::Unknown);
    CK_CHECK(c->calls == 1);
    auto precreated = std::make_shared<Control>();
    CK_CHECK(DirectoryIndexStore::create_new(factory_for(precreated), budget, context()).state() ==
             ObservationState::Unknown);
    CK_CHECK(precreated->closed == 0 && precreated->sizes == 0 && budget->charged_files() == 1);
    store.reset();
    CK_CHECK(c->file->closed == 1 && budget->charged_files() == 0);
    auto adopted = take(DirectoryIndexStore::create_new(factory_for(precreated), budget, context()));
    CK_CHECK(DirectoryIndexStore::create_new(factory, budget, context()).state() == ObservationState::Unknown);
    CK_CHECK(c->calls == 1);
    adopted.reset();
    CK_CHECK(budget->charged_files() == 0);
}
CK_TEST(index_store_factory_invalid_input_and_early_stop_never_invoke_creation) {
    auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
    auto c = std::make_shared<FactoryControl>();
    auto factory = std::make_shared<FakeFactory>(c);
    CK_CHECK(!DirectoryIndexStore::create_new({}, budget, context()).value());
    CK_CHECK(!DirectoryIndexStore::create_new(factory, {}, context()).value());
    auto invalid = context();
    invalid.generation.fill(std::byte{});
    CK_CHECK(!DirectoryIndexStore::create_new(factory, budget, invalid).value());
    CK_CHECK(!DirectoryIndexStore::create_new(factory, budget, context(), {0, 1024, {}}).value());
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(!DirectoryIndexStore::create_new(factory, budget, context(), {}, stop.get_token()).value());
    CK_CHECK(c->calls == 0 && budget->charged_files() == 0 && budget->charged_bytes() == 0);
}
CK_TEST(index_store_factory_preserves_all_failure_states_and_releases_unused_slot) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
        auto c = std::make_shared<FactoryControl>();
        auto factory = std::make_shared<FakeFactory>(c);
        c->error = UnavailableObservation{state, {ErrorDomain::Win32, 112, "create"}};
        c->on_create = [&] { CK_CHECK(budget->charged_files() == 1); };
        const auto result = DirectoryIndexStore::create_new(factory, budget, context());
        CK_CHECK(result.state() == state && result.unavailable_detail()->error == c->error->error);
        CK_CHECK(c->file->closed == 0 && budget->charged_files() == 0);
        c->error.reset();
        CK_CHECK(DirectoryIndexStore::create_new(factory, budget, context()).value());
        CK_CHECK(budget->charged_files() == 0);
    }
}
CK_TEST(index_store_factory_rejects_null_nonempty_and_failed_size_without_partial_owner) {
    for (int mode = 0; mode != 3; ++mode) {
        auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
        auto c = std::make_shared<FactoryControl>();
        auto factory = std::make_shared<FakeFactory>(c);
        if (mode == 0)
            c->null_file = true;
        if (mode == 1)
            c->file->data.push_back(std::byte{1});
        if (mode == 2)
            c->file->size_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "size"}};
        c->file->on_close = [&] { CK_CHECK(budget->charged_files() == 1); };
        const auto result = DirectoryIndexStore::create_new(factory, budget, context());
        CK_CHECK(!result.value() && budget->charged_files() == 0);
        CK_CHECK(c->file->closed == (mode == 0 ? 0U : 1U));
    }
}
CK_TEST(index_store_factory_cancellation_during_creation_or_size_closes_before_slot_release) {
    for (bool during_size : {false, true}) {
        auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
        auto c = std::make_shared<FactoryControl>();
        auto factory = std::make_shared<FakeFactory>(c);
        std::stop_source stop;
        auto request = [&] { stop.request_stop(); };
        if (during_size)
            c->file->on_size = request;
        else
            c->on_created = request;
        c->file->on_close = [&] { CK_CHECK(budget->charged_files() == 1); };
        CK_CHECK(!DirectoryIndexStore::create_new(factory, budget, context(), {}, stop.get_token()).value());
        CK_CHECK(c->calls == 1 && c->file->closed == 1 && budget->charged_files() == 0);
    }
}
CK_TEST(index_store_factory_exceptions_unwind_file_before_slot_and_keep_factory_live) {
    for (int phase = 0; phase != 3; ++phase) {
        auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
        auto c = std::make_shared<FactoryControl>();
        auto factory = std::make_shared<FakeFactory>(c);
        const std::weak_ptr<FakeFactory> weak = factory;
        auto fail = [] { throw std::runtime_error("injected factory/size failure"); };
        if (phase == 0)
            c->on_create = fail;
        if (phase == 1)
            c->on_created = fail;
        if (phase == 2)
            c->file->on_size = fail;
        c->file->on_close = [&] { CK_CHECK(budget->charged_files() == 1 && !weak.expired()); };
        bool threw = false;
        try {
            (void)DirectoryIndexStore::create_new(std::move(factory), budget, context());
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CK_CHECK(threw && budget->charged_files() == 0 && weak.expired());
        CK_CHECK(c->file->closed == (phase == 0 ? 0U : 1U));
    }
}
CK_TEST(index_store_retains_factory_until_file_closure_then_releases_both_charges) {
    auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
    auto c = std::make_shared<FactoryControl>();
    auto factory = std::make_shared<FakeFactory>(c);
    const std::weak_ptr<FakeFactory> weak = factory;
    auto store = take(DirectoryIndexStore::create_new(std::move(factory), budget, context()));
    CK_CHECK(!weak.expired());
    CK_CHECK(store->append(row()).value());
    c->file->on_close = [&] {
        CK_CHECK(!weak.expired() && budget->charged_files() == 1 && budget->charged_bytes() > 0);
    };
    c->on_close = [&] {
        CK_CHECK(c->file->closed == 1 && budget->charged_files() == 1 && budget->charged_bytes() > 0);
    };
    store.reset();
    CK_CHECK(weak.expired() && c->closed == 1 && budget->charged_files() == 0 && budget->charged_bytes() == 0);
}
CK_TEST(index_store_factory_reentrant_creation_cannot_bypass_inflight_file_limit) {
    auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
    auto c = std::make_shared<FactoryControl>();
    auto factory = std::make_shared<FakeFactory>(c);
    c->on_create = [&] {
        CK_CHECK(DirectoryIndexStore::create_new(factory, budget, context()).state() == ObservationState::Unknown);
        CK_CHECK(c->calls == 1);
    };
    auto store = take(DirectoryIndexStore::create_new(factory, budget, context()));
    CK_CHECK(c->calls == 1 && budget->charged_files() == 1);
    c->on_create = {};
}
