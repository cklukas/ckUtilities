// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "object_inventory_job.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <new>

namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;
using Op = FS::Operation;
using State = ObjectInventoryJobState;
using Inventory = ck::testing::SimulatedInventory;
template<class T> T take(Observation<T> observation) {
    if (!observation.value()) throw std::runtime_error("required object inventory fixture unavailable");
    return std::move(observation).take_value();
}
NativeName name() { return *NativeName::from_posix("file"); }
NativeFeatureIdentifier id(std::string text = "user.a") { return *NativeFeatureIdentifier::from_posix(std::move(text)); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-object-inventory";
#else
    return "/simulated-object-inventory";
#endif
}
UnavailableObservation denied() { return {ObservationState::Denied, {ErrorDomain::Adapter, 73, "inventory.denied"}}; }
struct Fixture {
    FS fs;
    FS::Pointer root = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
    Fixture() { fs.bind(path(), root); fs.put(root, name(), file); fs.metadata(file).logical_size = Observation<std::uint64_t>::available(3); configure(); }
    Inventory replies(std::size_t attributes = 1) {
        Inventory result; const auto owner = fs.metadata(file).identity;
        StorageInfo storage; storage.owner = owner; storage.model = StorageModel::LinuxStatx;
        result.storage = Observation<StorageInfo>::available(std::move(storage));
        result.link = Observation<LinkMetadata>::available({owner, PosixSymlinkData{"../target"}});
        DataChannelCatalog channels; channels.owner = owner; channels.coverage = DataChannelCoverage::WindowsFileStreamInfo;
        channels.channels.push_back({*NativeFeatureIdentifier::from_windows(u":a:$DATA"), DataChannelKind::NamedData,
            Observation<std::uint64_t>::available(9), {}});
        result.channels = Observation<DataChannelCatalog>::available(std::move(channels));
        AttributeCatalog catalog; catalog.owner = owner; catalog.coverage = AttributeCoverage::CredentialsVisiblePosix;
        for (std::size_t i = 0; i < attributes; ++i) {
            auto key = attributes == 1 ? id() : id("user." + std::to_string(i));
            catalog.attributes.push_back({key, AttributeRole::Opaque, Observation<std::uint64_t>::available(2), {}});
            result.values.emplace(key, Observation<AttributeValue>::available({key, {std::byte{0}, std::byte{0xff}}, owner, {}}));
        }
        result.attributes = Observation<AttributeCatalog>::available(std::move(catalog));
        for (const auto part : inventory_security_parts) result.security.emplace(part,
            Observation<NativeSecurityValue>::available({part, SecurityEncoding::Unknown, SecurityPresence::Present, {std::byte{5}}, owner, 0}));
        return result;
    }
    void configure(std::size_t count = 1) { fs.set_inventory(file, replies(count)); }
    std::unique_ptr<OpenedEntry> open() {
        return take(take(fs.open_directory(path()))->open_child(name(), EntryAccess::MetadataAndAttributes | EntryAccess::MetadataAndFullSecurity));
    }
    std::size_t calls(Op operation) const {
        return static_cast<std::size_t>(std::count_if(fs.calls().begin(), fs.calls().end(), [&](const auto& call) { return call.operation == operation; }));
    }
};
ObjectInventoryJob::Result finish(ObjectInventoryJob& job) {
    for (int i = 0; i < 100; ++i) {
        const auto state = job.advance();
        if (state == State::Complete) return take(job.completed());
        if (state == State::Failed) throw std::runtime_error("object inventory job failed unexpectedly");
    }
    throw std::runtime_error("object inventory fixture exceeded advance bound");
}
constexpr std::array operations{Op::InspectHandle, Op::InspectStorage, Op::ReadLink, Op::InspectChannels,
    Op::InspectAttributes, Op::ReadAttribute, Op::ReadSecurity};
}
CK_TEST(object_inventory_collects_all_families_and_pins_immutable_results_until_worker_retirement) {
    Fixture f; ObjectInventoryJob job(f.open(), {}, f.fs.clock()); auto result = finish(job);
    const auto& data = result->data();
    CK_CHECK(take(data.before.value()->logical_size) == 3 && take(data.before.value()->kind) == ObjectKind::RegularFile);
    CK_CHECK(take(data.identity_continuity) == IdentityComparison::Same);
    CK_CHECK(data.storage.value() && data.link.value() && data.channels.value() && data.attributes.value());
    CK_CHECK(data.attribute_values.size() == 1 && data.attribute_values[0].catalog_index == 0);
    CK_CHECK(take(data.attribute_values[0].value).identifier == id() && take(data.attribute_values[0].value).bytes.size() == 2);
    for (std::size_t i = 0; i < data.security.size(); ++i) CK_CHECK(take(data.security[i]).part == inventory_security_parts[i]);
    CK_CHECK(take(result->compare_owner(take(data.storage).owner)) == IdentityComparison::Same && f.calls(Op::ReadSecurity) == 6);
    CK_CHECK(f.fs.live_cursors() == 0 && f.fs.live_entries() == 1);
    const auto calls = f.fs.calls().size(); CK_CHECK(job.advance() == State::Complete && f.fs.calls().size() == calls);
    CK_CHECK(job.abandon() && !job.completed().value() && f.fs.live_entries() == 1);
    result.reset(); CK_CHECK(f.fs.live_entries() == 0 && !job.abandon());
}
CK_TEST(object_inventory_preserves_all_query_failures_without_inventing_absent_features) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
        ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        Fixture f;
        for (auto operation : operations) {
            const auto count = operation == Op::InspectHandle ? 2 : operation == Op::ReadSecurity ? 6 : 1;
            for (int i = 0; i < count; ++i) f.fs.enqueue(operation, {UnavailableObservation{state, denied().error}, {}, {}, false});
        }
        ObjectInventoryJob job(f.open(), {}, f.fs.clock()); const auto result = finish(job); const auto& data = result->data();
        CK_CHECK(data.before.state() == state && data.after.state() == state && data.storage.state() == state);
        CK_CHECK(data.link.state() == state && data.channels.state() == state && data.attributes.state() == state);
        CK_CHECK(data.attribute_values.empty() && f.calls(Op::ReadAttribute) == 0 && data.identity_continuity.state() == state);
        for (const auto& security : data.security) CK_CHECK(security.state() == state && security.unavailable_detail()->error == denied().error);
        CK_CHECK(result->compare_owner(f.fs.metadata(f.file).identity).state() == state);
    }
}
CK_TEST(object_inventory_attribute_value_denial_is_retained_independently_of_its_catalog) {
    Fixture f; f.fs.enqueue(Op::ReadAttribute, {denied(), {}, {}, false});
    ObjectInventoryJob job(f.open(), {}, f.fs.clock()); const auto result = finish(job); const auto& data = result->data();
    CK_CHECK(data.attributes.value() && data.attribute_values.size() == 1 && data.attribute_values[0].value.state() == ObservationState::Denied);
    CK_CHECK(data.attribute_values[0].value.unavailable_detail()->error == denied().error && f.calls(Op::ReadSecurity) == 6);
}
CK_TEST(object_inventory_cancellation_at_every_query_boundary_preserves_committed_private_work_without_retries) {
    for (const auto operation : operations) {
        Fixture f; std::stop_source stop;
        f.fs.enqueue(operation, {{}, {}, [&] { stop.request_stop(); }, false});
        ObjectInventoryJob job(f.open(), {}, f.fs.clock());
        CK_CHECK(job.advance(stop.get_token()) == State::Paused && !job.completed().value() && f.fs.live_entries() == 1);
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance(stop.get_token()) == State::Paused && f.fs.calls().size() == calls);
        const auto result = finish(job); CK_CHECK(result->data().attribute_values.size() == 1);
        for (const auto query : operations) CK_CHECK(f.calls(query) == (query == Op::InspectHandle ? 2 : query == Op::ReadSecurity ? 6 : 1));
    }
}
CK_TEST(object_inventory_slow_native_query_yields_after_committing_without_repeating_it) {
    Fixture f; f.fs.enqueue(Op::InspectStorage, {{}, std::chrono::milliseconds(25), {}, false});
    ObjectInventoryJob job(f.open(), {}, f.fs.clock());
    CK_CHECK(job.advance() == State::Collecting && f.calls(Op::InspectStorage) == 1 && f.calls(Op::ReadLink) == 0 && !job.completed().value());
    CK_CHECK(finish(job)->data().link.value() && f.calls(Op::InspectStorage) == 1);
}
CK_TEST(object_inventory_large_catalog_validation_and_value_reads_are_step_bounded_without_partial_publication) {
    Fixture f; f.configure(300); ObjectInventoryJob job(f.open(), {}, f.fs.clock());
    std::size_t advances = 0;
    while (job.state() != State::Complete && advances < 20) {
        const auto calls = f.fs.calls().size(); const auto state = job.advance();
        CK_CHECK(state != State::Failed && f.fs.calls().size() - calls <= ObjectInventoryJob::steps_per_advance);
        if (state != State::Complete) CK_CHECK(!job.completed().value());
        ++advances;
    }
    CK_CHECK(advances >= 3 && job.state() == State::Complete && take(job.completed())->data().attribute_values.size() == 300);
    CK_CHECK(f.calls(Op::ReadAttribute) == 300 && f.calls(Op::ReadSecurity) == 6);
}
CK_TEST(object_inventory_malformed_catalogs_values_security_and_link_replies_fail_without_a_result) {
    for (int mode = 0; mode < 8; ++mode) {
        Fixture f; auto replies = f.replies();
        if (mode == 0 || mode == 6) {
            auto catalog = take(*replies.channels);
            if (mode == 0) catalog.channels.push_back(catalog.channels.front()); else catalog.channels[0].kind = static_cast<DataChannelKind>(99);
            replies.channels = Observation<DataChannelCatalog>::available(std::move(catalog));
        } else if (mode == 1 || mode == 7) {
            auto catalog = take(*replies.attributes);
            if (mode == 1) catalog.attributes.push_back(catalog.attributes.front()); else catalog.attributes[0].role = static_cast<AttributeRole>(99);
            replies.attributes = Observation<AttributeCatalog>::available(std::move(catalog));
        } else if (mode == 2) replies.values.insert_or_assign(id(), Observation<AttributeValue>::available({id("wrong"), {}, {}, {}}));
        else if (mode == 3) replies.security.insert_or_assign(SecurityPart::LinuxAccessAcl,
            Observation<NativeSecurityValue>::available({SecurityPart::WindowsAccess, {}, {}, {}, {}, 0}));
        else if (mode == 4) replies.link = Observation<LinkMetadata>::available({{}, std::monostate{}});
        else {
            auto catalog = take(*replies.attributes); catalog.coverage = static_cast<AttributeCoverage>(99);
            replies.attributes = Observation<AttributeCatalog>::available(std::move(catalog));
        }
        f.fs.set_inventory(f.file, std::move(replies)); ObjectInventoryJob job(f.open(), {}, f.fs.clock());
        for (int i = 0; i < 10 && job.state() == State::Collecting; ++i) (void)job.advance();
        CK_CHECK(job.state() == State::Failed && !job.completed().value() && f.fs.live_entries() == 0 && job.failure());
        if (mode == 2) CK_CHECK(job.failure()->state == ObservationState::Changed && job.failure()->error.code == static_cast<std::int64_t>(AdapterError::MetadataChanged));
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance() == State::Failed && f.fs.calls().size() == calls && !job.abandon());
    }
}
CK_TEST(object_inventory_cumulative_limits_fail_before_unbounded_retention_and_leave_no_held_object) {
    for (bool records : {false, true}) {
        Fixture f; ObjectInventoryLimits limits;
        if (records) limits.retained.max_records = 1; else limits.retained.max_native_bytes = 8;
        ObjectInventoryJob job(f.open(), limits, f.fs.clock());
        CK_CHECK(job.advance() == State::Failed && job.failure()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
        CK_CHECK(!job.completed().value() && f.fs.live_entries() == 0);
        if (records) CK_CHECK(f.calls(Op::InspectHandle) == 1 && f.calls(Op::InspectStorage) == 0);
    }
}
CK_TEST(object_inventory_decoded_windows_link_names_count_toward_retained_bytes_separately_from_raw_query_bytes) {
    Fixture f; auto replies = f.replies(); WindowsReparseData link;
    link.raw = {std::byte{5}}; link.names = WindowsLinkNames{std::u16string(40, u'x'), u"print", 0};
    replies.link = Observation<LinkMetadata>::available({{}, std::move(link)}); f.fs.set_inventory(f.file, std::move(replies));
    ObjectInventoryLimits limits; limits.retained.max_native_bytes = 64;
    ObjectInventoryJob job(f.open(), limits, f.fs.clock()); CK_CHECK(job.advance() == State::Failed && f.fs.live_entries() == 0);
    CK_CHECK(job.failure()->error.operation == "object_inventory.link_names");
}
CK_TEST(object_inventory_owner_checks_and_identity_continuity_never_imply_a_snapshot) {
    Fixture f; auto replies = f.replies(); auto storage = take(*replies.storage);
    storage.owner = f.fs.metadata(f.root).identity; replies.storage = Observation<StorageInfo>::available(storage);
    f.fs.set_inventory(f.file, std::move(replies));
    f.fs.enqueue(Op::InspectHandle, {});
    f.fs.enqueue(Op::InspectHandle, {{}, {}, [&] { f.fs.metadata(f.file).logical_size = Observation<std::uint64_t>::available(99); }, false});
    ObjectInventoryJob job(f.open(), {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(take(result->data().identity_continuity) == IdentityComparison::Same);
    CK_CHECK(take(result->data().before.value()->logical_size) == 3 && take(result->data().after.value()->logical_size) == 99);
    CK_CHECK(take(result->compare_owner(storage.owner)) == IdentityComparison::Different);
    CK_CHECK(result->compare_owner({}).state() == ObservationState::Unknown);
}
CK_TEST(object_inventory_changed_or_unknown_object_ids_remain_explicit_even_when_queries_finish) {
    for (bool unknown : {false, true}) {
        Fixture f; f.fs.enqueue(Op::InspectHandle, {});
        f.fs.enqueue(Op::InspectHandle, {{}, {}, [&] {
            f.fs.metadata(f.file).identity = unknown ? Observation<ObjectIdentity>{} : f.fs.metadata(f.root).identity;
        }, false});
        ObjectInventoryJob job(f.open(), {}, f.fs.clock()); const auto result = finish(job);
        if (unknown) CK_CHECK(result->data().identity_continuity.state() == ObservationState::Unknown);
        else CK_CHECK(take(result->data().identity_continuity) == IdentityComparison::Different);
    }
}
CK_TEST(object_inventory_exceptions_including_bad_alloc_poison_and_release_without_allocating_error_text) {
    for (const auto operation : operations) {
        Fixture f; f.fs.enqueue(operation, {{}, {}, [] { throw std::bad_alloc{}; }, false});
        ObjectInventoryJob job(f.open(), {}, f.fs.clock()); bool caught = false;
        try { (void)job.advance(); } catch (const std::bad_alloc&) { caught = true; }
        CK_CHECK(caught && job.state() == State::Failed && job.failure()->error.operation.empty() && f.fs.live_entries() == 0);
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance() == State::Failed && f.fs.calls().size() == calls && !job.completed().value());
    }
}
CK_TEST(object_inventory_clock_reversal_and_throwing_clocks_poison_instead_of_exposing_private_results) {
    for (bool throwing : {false, true}) {
        Fixture f; f.fs.enqueue(Op::InspectHandle, {{}, std::chrono::milliseconds(25), {}, false});
        ObjectInventoryJob job(f.open(), {}, f.fs.clock()); CK_CHECK(job.advance() == State::Collecting);
        if (throwing) f.fs.clock()->throw_on_read = true; else f.fs.clock()->value = MonotonicClock::TimePoint{} - std::chrono::seconds(1);
        bool caught = false; try { (void)job.advance(); } catch (const std::runtime_error&) { caught = true; }
        CK_CHECK(caught == throwing && job.state() == State::Failed && f.fs.live_entries() == 0 && !job.completed().value());
    }
}
CK_TEST(object_inventory_abandonment_closes_private_work_and_already_stopped_calls_make_no_queries) {
    Fixture f; std::stop_source stop; stop.request_stop(); ObjectInventoryJob job(f.open(), {}, f.fs.clock());
    const auto calls = f.fs.calls().size(); CK_CHECK(job.advance(stop.get_token()) == State::Paused && f.fs.calls().size() == calls);
    CK_CHECK(job.abandon() && job.state() == State::Abandoned && f.fs.live_entries() == 0 && !job.completed().value());
    CK_CHECK(job.advance() == State::Abandoned && f.fs.calls().size() == calls);
}
CK_TEST(object_inventory_constructor_refuses_invalid_owners_clocks_limits_and_slices_before_queries) {
    for (int mode = 0; mode < 7; ++mode) {
        Fixture f; ObjectInventoryLimits limits; auto clock = f.fs.clock();
        if (mode == 2) limits.query.max_native_bytes = 0;
        if (mode == 3) limits.retained.max_records = InventoryLimits::hard_records + 1;
        if (mode == 4) limits.time_slice = MonotonicClock::Duration::zero();
        if (mode == 5) limits.time_slice = std::chrono::seconds(2);
        if (mode == 6) limits.retained.max_native_bytes = 0;
        auto opened = mode == 0 ? std::unique_ptr<OpenedEntry>{} : f.open(); const auto calls = f.fs.calls().size();
        bool caught = false; try { ObjectInventoryJob job(std::move(opened), limits, mode == 1 ? nullptr : clock); }
        catch (const std::invalid_argument&) { caught = true; }
        CK_CHECK(caught && f.fs.calls().size() == calls && f.fs.live_entries() == 0);
    }
}
CK_TEST(object_inventory_native_query_limits_preserve_refusals_as_observations_without_returning_value_prefixes) {
    Fixture f; auto replies = f.replies(); replies.values.insert_or_assign(id(),
        Observation<AttributeValue>::available({id(), NativeBytes(32, std::byte{1}), f.fs.metadata(f.file).identity, {}}));
    f.fs.set_inventory(f.file, std::move(replies)); ObjectInventoryLimits limits; limits.query.max_native_bytes = 16;
    ObjectInventoryJob job(f.open(), limits, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(result->data().attribute_values.size() == 1 && result->data().attribute_values[0].value.state() == ObservationState::Failed);
    CK_CHECK(!result->data().attribute_values[0].value.value() && result->data().security[0].value());
}
namespace {
class OversizedReply final : public OpenedEntry {
public:
    OversizedReply(int mode, int& owners) : mode_(mode), owners_(owners) { ++owners_; }
    ~OversizedReply() override { --owners_; }
    Observation<BasicMetadata> inspect_basic() const override { return Observation<BasicMetadata>::available({}); }
    Observation<LinkMetadata> read_link_metadata(InventoryLimits) const override {
        return Observation<LinkMetadata>::available({{}, PosixSymlinkData{std::string(mode_ == 2 ? 17 : 1, 'x')}});
    }
    Observation<DataChannelCatalog> inspect_data_channels(InventoryLimits) const override {
        DataChannelCatalog catalog;
        if (mode_ == 0) catalog.channels.assign(3, {id(), DataChannelKind::NamedData, {}, {}});
        if (mode_ == 5) catalog.coverage = static_cast<DataChannelCoverage>(99);
        return Observation<DataChannelCatalog>::available(std::move(catalog));
    }
    Observation<AttributeCatalog> inspect_attributes(InventoryLimits) const override {
        AttributeCatalog catalog; catalog.attributes.assign(mode_ == 1 ? 3 : 1, {id(), AttributeRole::Opaque, {}, {}});
        return Observation<AttributeCatalog>::available(std::move(catalog));
    }
    Observation<AttributeValue> read_attribute(const NativeFeatureIdentifier& key, InventoryLimits) const override {
        return Observation<AttributeValue>::available({key, NativeBytes(mode_ == 3 ? 17 : 1), {}, {}});
    }
    Observation<NativeSecurityValue> read_security(SecurityPart part, InventoryLimits) const override {
        return Observation<NativeSecurityValue>::available({part, SecurityEncoding::Unknown, SecurityPresence::Present,
            NativeBytes(mode_ == 4 ? 17 : 1), {}, 0});
    }
private: int mode_; int& owners_;
};
}
CK_TEST(object_inventory_refuses_replies_that_violate_query_budgets_even_if_an_adapter_ignores_them) {
    for (int mode = 0; mode < 6; ++mode) {
        int owners = 0; ObjectInventoryLimits limits; limits.query = {16, 2};
        ObjectInventoryJob job(std::make_unique<OversizedReply>(mode, owners), limits);
        for (int i = 0; i < 10 && job.state() == State::Collecting; ++i) (void)job.advance();
        CK_CHECK(job.state() == State::Failed && !job.completed().value() && owners == 0);
        CK_CHECK(job.failure()->error.code == static_cast<std::int64_t>(AdapterError::InvalidNativeResult));
    }
}
CK_TEST(object_inventory_attribute_identifiers_keep_encoding_namespaces_and_raw_units_separate) {
    Fixture f; auto replies = f.replies(); AttributeCatalog catalog;
    const std::array keys{id("same"), *NativeFeatureIdentifier::from_windows_ea("same"), *NativeFeatureIdentifier::from_windows(u"same"),
        *NativeFeatureIdentifier::from_windows(std::u16string{u'x', char16_t{0xd800}}), id("raw\xff")};
    replies.values.clear();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        catalog.attributes.push_back({keys[i], AttributeRole::Opaque, {}, {}});
        replies.values.emplace(keys[i], Observation<AttributeValue>::available({keys[i], {static_cast<std::byte>(i)}, {}, {}}));
    }
    replies.attributes = Observation<AttributeCatalog>::available(std::move(catalog)); f.fs.set_inventory(f.file, std::move(replies));
    ObjectInventoryJob job(f.open(), {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(result->data().attribute_values.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto value = take(result->data().attribute_values[i].value);
        CK_CHECK(value.identifier == keys[i] && value.bytes == NativeBytes{static_cast<std::byte>(i)});
        CK_CHECK(result->compare_owner(value.owner).state() == ObservationState::Unknown);
    }
}
CK_TEST(object_inventory_retained_identifier_budget_is_independent_of_each_native_query_buffer_budget) {
    // Native families have different buffer layouts: a small value query can
    // legitimately return a value for a much longer externally supplied key.
    class Entry final : public OpenedEntry {
    public:
        Observation<BasicMetadata> inspect_basic() const override { return Observation<BasicMetadata>::available({}); }
        Observation<AttributeCatalog> inspect_attributes(InventoryLimits) const override {
            AttributeCatalog catalog; catalog.attributes.push_back({id(std::string(100, 'x')), AttributeRole::Opaque, {}, {}});
            return Observation<AttributeCatalog>::available(std::move(catalog));
        }
        Observation<AttributeValue> read_attribute(const NativeFeatureIdentifier& key, InventoryLimits limits) const override {
            if (limits.max_native_bytes != 1) throw std::runtime_error("query budget was changed");
            return Observation<AttributeValue>::available({key, {std::byte{1}}, {}, {}});
        }
    };
    ObjectInventoryLimits limits; limits.query.max_native_bytes = 1;
    ObjectInventoryJob job(std::make_unique<Entry>(), limits); const auto result = finish(job);
    CK_CHECK(result->data().attribute_values.size() == 1 && take(result->data().attribute_values[0].value).identifier == id(std::string(100, 'x')));
}
CK_TEST(object_inventory_keeps_additional_data_attribute_roles_and_bytes_explicit) {
    Fixture f; auto replies = f.replies(); auto catalog = take(*replies.attributes);
    catalog.attributes[0].role = AttributeRole::AdditionalData;
    replies.attributes = Observation<AttributeCatalog>::available(std::move(catalog)); f.fs.set_inventory(f.file, std::move(replies));
    ObjectInventoryJob job(f.open(), {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(result->data().attributes.value()->attributes[0].role == AttributeRole::AdditionalData);
    const NativeBytes expected{std::byte{0}, std::byte{0xff}};
    CK_CHECK(take(result->data().attribute_values[0].value).bytes == expected && f.calls(Op::ReadAttribute) == 1);
}
