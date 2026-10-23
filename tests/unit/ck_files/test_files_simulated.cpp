// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "simulated_filesystem.hpp"
#include "pane_listing_job.hpp"
#include "cvision/testing/cktest.hpp"
#include <array>
#include <type_traits>
namespace {
using namespace ck::files;
using FileSystem = ck::testing::SimulatedFileSystem;
using Operation = FileSystem::Operation;
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-files";
#else
    return "/simulated-files";
#endif
}
NativeName name(const char* text) { return *NativeName::from_posix(text); }
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("simulated fixture observation unavailable");
    return std::move(result).take_value();
}
UnavailableObservation denied() { return {ObservationState::Denied, {ErrorDomain::Adapter, 53, "injected.denied"}}; }
FileSystem::Pointer root(FileSystem& fs) { auto node = fs.make_node(ObjectKind::Directory); fs.bind(path(), node); return node; }
PaneListingJob::Result finish(PaneListingJob& job) {
    for (int i = 0; i < 64; ++i) {
        if (job.advance() == PaneListingJobState::Complete) return take(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("simulated listing failed");
    }
    throw std::runtime_error("simulated listing exceeded step budget");
}
}
CK_TEST(simulated_filesystem_streams_raw_posix_and_windows_names_into_panes_without_host_io) {
    for (bool windows : {false, true}) {
        FileSystem fs; auto directory = root(fs);
        auto raw = windows ? *NativeName::from_windows(std::u16string{u'x', char16_t{0xd800}}) : *NativeName::from_posix(std::string{"x\xff", 2});
        fs.put(directory, raw, fs.make_node(ObjectKind::RegularFile));
        PaneModel pane;
        PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown);
        CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.size() == 1);
        CK_CHECK(pane.visible_row(0) && pane.visible_row(0)->entry.name == raw && pane.visible_row(0)->display_name == raw.display_name());
        CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 0);
    }
}
CK_TEST(simulated_path_replacement_keeps_old_cursor_and_child_handles_on_original_objects) {
    FileSystem fs; auto old = root(fs); auto file = fs.make_node(ObjectKind::RegularFile); fs.put(old, name("a"), file);
    auto cursor = take(fs.open_directory(path())); auto held = take(cursor->open_child(name("a")));
    const auto original = take(take(held->inspect_basic()).identity);
    auto replacement = fs.make_node(ObjectKind::Directory); fs.bind(path(), replacement); fs.unbind(path());
    CK_CHECK(fs.open_directory(path()).state() == ObservationState::Gone);
    CK_CHECK(cursor->read(4).names == std::vector<NativeName>{name("a")});
    fs.erase(old, name("a")); CK_CHECK(cursor->open_child(name("a")).state() == ObservationState::Gone);
    CK_CHECK(compare_identity(take(take(held->inspect_basic()).identity), original) == IdentityComparison::Same);
    cursor.reset(); CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 1);
    held.reset(); CK_CHECK(fs.live_entries() == 0);
}
CK_TEST(simulated_enumeration_names_and_live_membership_can_disagree_without_hiding_gone_rows) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("gone"), fs.make_node(ObjectKind::RegularFile));
    auto listing = take(DirectoryListing::open(fs, path(), {}, fs.clock()));
    fs.erase(directory, name("gone")); fs.put(directory, name("later"), fs.make_node(ObjectKind::RegularFile));
    const auto batch = listing->read();
    CK_CHECK(batch.state == DirectoryReadState::End && batch.entries.size() == 1);
    CK_CHECK(batch.entries.size() == 1 && batch.entries[0].name == name("gone") && batch.entries[0].metadata.state() == ObservationState::Gone);
}
CK_TEST(simulated_faults_retain_unknown_denied_and_native_error_details) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::OpenDirectory, {denied(), {}, {}, false});
    const auto refused = DirectoryListing::open(fs, path(), {}, fs.clock());
    CK_CHECK(refused.state() == ObservationState::Denied && refused.unavailable_detail()->error == denied().error);
    fs.enqueue(Operation::InspectHandle, {UnavailableObservation{ObservationState::Unknown, {ErrorDomain::Adapter, 54, "unknown.inspect"}}, {}, {}, false});
    auto listing = take(DirectoryListing::open(fs, path(), {}, fs.clock())); const auto batch = listing->read();
    CK_CHECK(batch.entries.size() == 1 && batch.entries[0].metadata.state() == ObservationState::Unknown);
    CK_CHECK(batch.entries.size() == 1 && batch.entries[0].metadata.unavailable_detail() && batch.entries[0].metadata.unavailable_detail()->error.operation == "unknown.inspect" && fs.live_entries() == 0);
}
CK_TEST(simulated_mid_scan_failure_poisoning_discards_private_pane_rows_and_closes_handles) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::ReadDirectory, {}); fs.enqueue(Operation::ReadDirectory, {denied(), {}, {}, false});
    PaneModel pane; PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate),
        take(DirectoryListing::open(fs, path(), {1,128,128}, fs.clock())), StorageModel::Unknown);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && job.advance() == PaneListingJobState::Failed);
    CK_CHECK(!job.completed().value() && job.failure() && job.failure()->error == denied().error && pane.size() == 0);
    const auto calls = fs.calls().size(); CK_CHECK(job.advance() == PaneListingJobState::Failed && fs.calls().size() == calls);
    CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 0);
}
CK_TEST(simulated_side_effect_cancellation_closes_prepared_objects_and_can_resume_exact_name) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    std::stop_source stop; fs.enqueue(Operation::OpenChild, {{}, {}, [&] { stop.request_stop(); }, false});
    auto listing = take(DirectoryListing::open(fs, path(), {}, fs.clock()));
    CK_CHECK(listing->read(stop.get_token()).state == DirectoryReadState::Cancelled && fs.live_entries() == 0);
    const auto resumed = listing->read();
    CK_CHECK(resumed.state == DirectoryReadState::End && resumed.entries.size() == 1 && resumed.entries[0].name == name("a"));
}
CK_TEST(simulated_latency_time_slices_keep_prepared_handles_and_abandonment_releases_them) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::OpenChild, {{}, std::chrono::milliseconds(20), {}, false});
    PaneModel pane; PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate),
        take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && fs.live_entries() == 1 && fs.live_cursors() == 1);
    CK_CHECK(job.abandon() && fs.live_entries() == 0 && fs.live_cursors() == 0);
    CK_CHECK(pane.cancel_listing(job.request()));
}
CK_TEST(simulated_exception_poisoning_releases_private_discovery_and_prevents_retries) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::InspectHandle, {{}, {}, {}, true});
    PaneModel pane; PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate),
        take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown);
    bool caught = false; try { (void)job.advance(); } catch (const std::runtime_error&) { caught = true; }
    CK_CHECK(caught && job.state() == PaneListingJobState::Failed && fs.live_entries() == 0 && fs.live_cursors() == 0);
    const auto calls = fs.calls().size(); CK_CHECK(job.advance() == PaneListingJobState::Failed && fs.calls().size() == calls);
}
CK_TEST(simulated_directory_children_own_independent_cursors_and_never_follow_link_nodes) {
    FileSystem fs; auto directory = root(fs); auto child = fs.make_node(ObjectKind::Directory);
    fs.put(directory, name("child"), child); fs.put(child, name("file"), fs.make_node(ObjectKind::RegularFile));
    fs.put(directory, name("link"), fs.make_node(ObjectKind::SymbolicLink));
    auto parent = take(fs.open_directory(path())); auto first = take(parent->open_directory_child(name("child")));
    auto second = take(parent->open_directory_child(name("child"))); parent.reset();
    CK_CHECK(first->read(8).names == std::vector<NativeName>{name("file")} && second->read(8).names == std::vector<NativeName>{name("file")});
    CK_CHECK(fs.live_cursors() == 2); auto fresh = take(fs.open_directory(path()));
    CK_CHECK(!fresh->open_directory_child(name("link")).value());
    CK_CHECK(take(take(take(fresh->open_child(name("link")))->inspect_basic()).kind) == ObjectKind::SymbolicLink);
}
CK_TEST(simulated_bindings_reject_foreign_nodes_relative_paths_and_directory_cycles) {
    FileSystem fs, foreign; auto directory = root(fs); auto child = fs.make_node(ObjectKind::Directory); fs.put(directory, name("child"), child);
    bool cycle = false, other = false, relative = false;
    try { fs.put(child, name("cycle"), directory); } catch (const std::invalid_argument&) { cycle = true; }
    try { fs.put(directory, name("foreign"), foreign.make_node(ObjectKind::RegularFile)); } catch (const std::invalid_argument&) { other = true; }
    try { fs.bind("relative", directory); } catch (const std::invalid_argument&) { relative = true; }
    CK_CHECK(cycle && other && relative && fs.calls().empty());
}
CK_TEST(simulated_held_refresh_qualifies_continuity_and_preserves_pane_selection) {
    FileSystem fs; auto directory = root(fs); auto object = fs.make_node(ObjectKind::RegularFile); fs.put(directory, name("a"), object);
    std::shared_ptr<const DirectoryCursor> parent(take(fs.open_directory(path())));
    auto held = take(HeldChild::observe(parent, name("a")));
    PaneModel pane; CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Navigate),
        {PaneEntry::from_held(held, Observation<bool>::available(false))}) == PanePublish::Applied);
    const auto id = pane.focused(); CK_CHECK(id && pane.set_mark(*id, true));
    fs.metadata(object).logical_size = Observation<std::uint64_t>::available(77);
    auto refreshed = take(held->refresh()); CK_CHECK(refreshed->continues(*held));
    CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Refresh),
        {PaneEntry::from_held(refreshed, Observation<bool>::available(false))}) == PanePublish::Applied);
    CK_CHECK(id && pane.focused() == id && pane.marked(*id) && pane.row(*id));
    if (!id || !pane.row(*id)) throw std::runtime_error("simulated refresh lost pane row");
    CK_CHECK(take(pane.row(*id)->entry.metadata.logical_size) == 77);
}
CK_TEST(simulated_held_refresh_detects_replacement_even_with_an_old_open_handle) {
    FileSystem fs; auto directory = root(fs); auto old = fs.make_node(ObjectKind::RegularFile); fs.put(directory, name("a"), old);
    std::shared_ptr<const DirectoryCursor> parent(take(fs.open_directory(path())));
    auto held = take(HeldChild::observe(parent, name("a")));
    fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    auto refreshed = take(held->refresh());
    CK_CHECK(!refreshed->continues(*held) && take(refreshed->previous_continuity()) == IdentityComparison::Different);
    CK_CHECK(fs.live_entries() == 2 && fs.live_cursors() == 1);
    held.reset(); refreshed.reset(); parent.reset(); CK_CHECK(fs.live_entries() == 0 && fs.live_cursors() == 0);
}

namespace {
using Inventory = ck::testing::SimulatedInventory;
NativeFeatureIdentifier feature(const char* text = "user.raw") { return *NativeFeatureIdentifier::from_posix(text); }
Inventory inventory(FileSystem& fs, const FileSystem::Pointer& object) {
    const auto owner = fs.metadata(object).identity;
    Inventory result;
    StorageInfo storage; storage.owner = owner; storage.model = StorageModel::LinuxStatx;
    storage.allocated_size = Observation<std::uint64_t>::available(4096);
    result.storage = Observation<StorageInfo>::available(storage);
    result.link = Observation<LinkMetadata>::available({owner, PosixSymlinkData{std::string{"../raw\xff", 7}}});
    DataChannelCatalog channels; channels.owner = owner; channels.coverage = DataChannelCoverage::WindowsFileStreamInfo;
    channels.channels.push_back({*NativeFeatureIdentifier::from_windows(u":extra:$DATA"), DataChannelKind::NamedData,
        Observation<std::uint64_t>::available(5), {}});
    result.channels = Observation<DataChannelCatalog>::available(std::move(channels));
    AttributeCatalog attributes; attributes.owner = owner; attributes.coverage = AttributeCoverage::CredentialsVisiblePosix;
    attributes.attributes.push_back({feature(), AttributeRole::Opaque, Observation<std::uint64_t>::available(3), {}});
    result.attributes = Observation<AttributeCatalog>::available(std::move(attributes));
    result.values.emplace(feature(), Observation<AttributeValue>::available({feature(), {std::byte{0}, std::byte{0xff}, std::byte{7}}, owner, {}}));
    for (auto part : {SecurityPart::LinuxAccessAcl, SecurityPart::LinuxDefaultAcl, SecurityPart::DarwinExtendedAcl,
        SecurityPart::WindowsAccess, SecurityPart::WindowsIntegrity, SecurityPart::WindowsFullDescriptor}) {
        // Opaque, deliberately not structurally valid native ACL data. Tests of
        // native decoders use their separate byte fixtures; this tests transport.
        result.security.emplace(part, Observation<NativeSecurityValue>::available({part, SecurityEncoding::Unknown,
            SecurityPresence::Present, {std::byte{9}, std::byte{0}}, owner, 0}));
    }
    return result;
}
std::unique_ptr<OpenedEntry> entry(FileSystem& fs, EntryAccess access = EntryAccess::MetadataAndAttributes | EntryAccess::MetadataAndFullSecurity) {
    return take(take(fs.open_directory(path()))->open_child(name("a"), access));
}
FileSystem::Pointer inventory_object(FileSystem& fs) {
    auto directory = root(fs); auto object = fs.make_node(ObjectKind::RegularFile);
    fs.put(directory, name("a"), object); fs.set_inventory(object, inventory(fs, object)); return object;
}
std::vector<ObservationState> inventory_states(const OpenedEntry& object) {
    return {object.inspect_storage().state(), object.read_link_metadata().state(), object.inspect_data_channels().state(),
        object.inspect_attributes().state(), object.read_attribute(feature()).state(), object.read_security(SecurityPart::WindowsAccess).state()};
}
constexpr std::array inventory_operations{Operation::InspectStorage, Operation::ReadLink, Operation::InspectChannels,
    Operation::InspectAttributes, Operation::ReadAttribute, Operation::ReadSecurity};
}
CK_TEST(simulated_inventory_roundtrips_all_query_families_and_exact_native_data_without_host_assumptions) {
    FileSystem fs; auto object = inventory_object(fs); auto held = entry(fs);
    for (auto state : inventory_states(*held)) CK_CHECK(state == ObservationState::Available);
    const auto storage = take(held->inspect_storage());
    CK_CHECK(take(storage.allocated_size) == 4096 && compare_identity(take(storage.owner), take(fs.metadata(object).identity)) == IdentityComparison::Same);
    const auto link = take(held->read_link_metadata());
    CK_CHECK(std::get<PosixSymlinkData>(link.payload).target == std::string("../raw\xff", 7));
    const auto channels = take(held->inspect_data_channels());
    CK_CHECK(channels.coverage == DataChannelCoverage::WindowsFileStreamInfo && channels.channels.size() == 1);
    CK_CHECK(channels.channels[0].identifier.windows_units() && *channels.channels[0].identifier.windows_units() == u":extra:$DATA");
    const auto attributes = take(held->inspect_attributes());
    CK_CHECK(attributes.coverage == AttributeCoverage::CredentialsVisiblePosix && attributes.attributes.size() == 1);
    const NativeBytes expected{std::byte{0}, std::byte{0xff}, std::byte{7}};
    CK_CHECK(take(held->read_attribute(feature())).bytes == expected);
    for (const auto& [part, reply] : inventory(fs, object).security) {
        const auto value = take(held->read_security(part));
        CK_CHECK(value.part == part && value.bytes == take(reply).bytes && value.encoding == SecurityEncoding::Unknown);
    }
    CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 1);
    held.reset(); CK_CHECK(fs.live_entries() == 0);
}
CK_TEST(simulated_inventory_unconfigured_queries_and_keys_are_unsupported_not_empty_or_absent) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    auto held = entry(fs);
    for (auto state : inventory_states(*held)) CK_CHECK(state == ObservationState::Unsupported);
}
CK_TEST(simulated_inventory_every_family_preserves_independent_failure_states_errors_and_actions) {
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
        ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        FileSystem fs; (void)inventory_object(fs); auto held = entry(fs);
        for (auto operation : inventory_operations) fs.enqueue(operation, {UnavailableObservation{state,
            {ErrorDomain::NtStatus, 0x1234, "inventory.injected"}}, std::chrono::milliseconds(3), {}, false});
        for (auto actual : inventory_states(*held)) CK_CHECK(actual == state);
        CK_CHECK(fs.clock()->now().time_since_epoch() == std::chrono::milliseconds(18));
        for (auto actual : inventory_states(*held)) CK_CHECK(actual == ObservationState::Available);
        fs.enqueue(Operation::ReadAttribute, {denied(), {}, {}, false});
        const auto value = held->read_attribute(feature()); CK_CHECK(value.unavailable_detail()->error == denied().error);
    }
}
CK_TEST(simulated_inventory_configured_unavailable_and_partial_fields_are_not_rewritten) {
    FileSystem fs; auto object = inventory_object(fs); auto data = inventory(fs, object);
    data.storage = Observation<StorageInfo>::unavailable(ObservationState::Denied, denied().error);
    auto channels = take(*data.channels);
    channels.channels[0].logical_size = Observation<std::uint64_t>::unavailable(ObservationState::Unknown, denied().error);
    channels.owner = Observation<ObjectIdentity>::unavailable(ObservationState::Changed, denied().error);
    data.channels = Observation<DataChannelCatalog>::available(std::move(channels));
    data.values.insert_or_assign(feature(), Observation<AttributeValue>::unavailable(ObservationState::Gone, denied().error));
    fs.set_inventory(object, std::move(data)); auto held = entry(fs);
    CK_CHECK(held->inspect_storage().state() == ObservationState::Denied && held->read_attribute(feature()).state() == ObservationState::Gone);
    const auto result = take(held->inspect_data_channels());
    CK_CHECK(result.owner.state() == ObservationState::Changed && result.channels[0].logical_size.state() == ObservationState::Unknown);
    CK_CHECK(take(held->inspect_attributes()).attributes.size() == 1);
}
CK_TEST(simulated_inventory_access_requests_are_recorded_and_do_not_grant_unrequested_reads) {
    FileSystem fs; (void)inventory_object(fs);
    for (auto access : {EntryAccess::MetadataOnly, EntryAccess::MetadataAndAttributes, EntryAccess::MetadataAndSecurity,
        EntryAccess::MetadataAndFullSecurity, EntryAccess::MetadataAndAttributes | EntryAccess::MetadataAndFullSecurity}) {
        auto held = entry(fs, access);
        CK_CHECK(fs.calls().back().operation == Operation::OpenChild && fs.calls().back().access == access);
        CK_CHECK(held->inspect_storage().value() && held->inspect_data_channels().value() && held->read_link_metadata().value());
        CK_CHECK(held->inspect_attributes().state() == (requests_attributes(access) ? ObservationState::Available : ObservationState::Denied));
        CK_CHECK(held->read_attribute(feature()).state() == (requests_attributes(access) ? ObservationState::Available : ObservationState::Denied));
        CK_CHECK(held->read_security(SecurityPart::WindowsAccess).state() == (requests_security(access) ? ObservationState::Available : ObservationState::Denied));
        CK_CHECK(held->read_security(SecurityPart::WindowsFullDescriptor).state() == (requests_audit_security(access) ? ObservationState::Available : ObservationState::Denied));
        CK_CHECK(fs.calls().back().access == access);
    }
    auto cursor = take(fs.open_directory(path())); const auto count = fs.calls().size();
    CK_CHECK(!cursor->open_child(name("a"), static_cast<EntryAccess>(4)).value() && fs.calls().size() == count);
}
CK_TEST(simulated_inventory_queries_apply_exact_byte_and_record_limits_without_partial_prefixes) {
    FileSystem fs; (void)inventory_object(fs); auto held = entry(fs);
    CK_CHECK(held->inspect_data_channels({24, 1}).value());
    CK_CHECK(held->inspect_data_channels({23, 1}).state() == ObservationState::Failed);
    CK_CHECK(held->inspect_attributes({8, 1}).value());
    CK_CHECK(held->inspect_attributes({7, 1}).state() == ObservationState::Failed);
    CK_CHECK(held->read_attribute(feature(), {3, 1}).value() && !held->read_attribute(feature(), {2, 1}).value());
    CK_CHECK(held->read_link_metadata({7, 1}).value() && !held->read_link_metadata({6, 1}).value());
    CK_CHECK(held->read_security(SecurityPart::WindowsAccess, {2, 1}).value());
    const auto failure = held->read_security(SecurityPart::WindowsAccess, {1, 1});
    CK_CHECK(failure.state() == ObservationState::Failed && failure.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
    const auto call = fs.calls().back();
    CK_CHECK(call.security == SecurityPart::WindowsAccess && call.limits && call.limits->max_native_bytes == 1 && call.limits->max_records == 1);
}
CK_TEST(simulated_inventory_invalid_query_limits_do_not_consume_faults_or_make_calls) {
    FileSystem fs; (void)inventory_object(fs); auto held = entry(fs); const auto calls = fs.calls().size();
    for (auto operation : inventory_operations) fs.enqueue(operation, {denied(), {}, {}, false});
    for (auto limits : {InventoryLimits{0, 1}, InventoryLimits{1, 0}, InventoryLimits{InventoryLimits::hard_native_bytes + 1, 1},
        InventoryLimits{1, InventoryLimits::hard_records + 1}}) {
        CK_CHECK(held->read_link_metadata(limits).state() == ObservationState::Failed);
        CK_CHECK(held->inspect_data_channels(limits).state() == ObservationState::Failed);
        CK_CHECK(held->inspect_attributes(limits).state() == ObservationState::Failed);
        CK_CHECK(held->read_attribute(feature(), limits).state() == ObservationState::Failed);
        CK_CHECK(held->read_security(SecurityPart::WindowsAccess, limits).state() == ObservationState::Failed);
    }
    CK_CHECK(held->read_security(SecurityPart::Unknown).state() == ObservationState::Failed && fs.calls().size() == calls);
    for (auto state : inventory_states(*held)) CK_CHECK(state == ObservationState::Denied);
}
CK_TEST(simulated_inventory_feature_keys_preserve_namespace_and_invalid_encoding_and_trace_requested_key) {
    FileSystem fs; auto object = inventory_object(fs); Inventory data;
    const std::array keys{feature("same"), *NativeFeatureIdentifier::from_windows_ea("same"),
        *NativeFeatureIdentifier::from_windows(u"same"), feature("raw\xff"),
        *NativeFeatureIdentifier::from_windows(std::u16string{u'x', char16_t{0xd800}})};
    for (std::size_t i = 0; i < keys.size(); ++i)
        data.values.emplace(keys[i], Observation<AttributeValue>::available({keys[i], {static_cast<std::byte>(i)}, {}, {}}));
    fs.set_inventory(object, std::move(data)); auto held = entry(fs);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto value = take(held->read_attribute(keys[i], {1, 1}));
        CK_CHECK(value.identifier == keys[i] && value.bytes.size() == 1 && value.bytes[0] == static_cast<std::byte>(i));
        CK_CHECK(fs.calls().back().feature == keys[i] && fs.calls().back().object == object->id());
    }
    CK_CHECK(held->read_attribute(feature("missing")).state() == ObservationState::Unsupported);
}
CK_TEST(simulated_inventory_handle_replacement_and_query_effects_keep_object_ownership_and_current_replies) {
    FileSystem fs; auto object = inventory_object(fs); auto held = entry(fs);
    auto replacement = fs.make_node(ObjectKind::RegularFile); auto directory = root(fs); fs.put(directory, name("a"), replacement);
    auto new_entry = entry(fs); CK_CHECK(new_entry->inspect_storage().state() == ObservationState::Unsupported);
    fs.enqueue(Operation::ReadAttribute, {{}, {}, [&] {
        auto data = inventory(fs, object);
        data.values.insert_or_assign(feature(), Observation<AttributeValue>::available({feature(), {std::byte{42}}, fs.metadata(object).identity, {}}));
        fs.set_inventory(object, std::move(data));
    }, false});
    CK_CHECK(take(held->read_attribute(feature())).bytes == NativeBytes{std::byte{42}});
    CK_CHECK(fs.calls().back().object == object->id());
    fs.enqueue(Operation::InspectChannels, {{}, {}, [&] {
        Inventory data; data.channels = Observation<DataChannelCatalog>::unavailable(ObservationState::Changed, denied().error);
        fs.set_inventory(object, std::move(data));
    }, false});
    CK_CHECK(held->inspect_data_channels().state() == ObservationState::Changed);
    held.reset(); new_entry.reset(); CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 0);
}
CK_TEST(simulated_inventory_configuration_rejects_cumulative_bytes_records_and_invalid_security_without_replacing_old_data) {
    FileSystem fs; auto object = inventory_object(fs); auto held = entry(fs);
    for (int scenario = 0; scenario < 3; ++scenario) {
        Inventory data;
        if (scenario == 0) {
            data.values.emplace(feature(), Observation<AttributeValue>::available({feature(), NativeBytes(InventoryLimits::hard_native_bytes), {}, {}}));
        } else if (scenario == 1) {
            AttributeCatalog attributes;
            attributes.attributes.assign(InventoryLimits::hard_records, {feature(), AttributeRole::Opaque, {}, {}});
            data.attributes = Observation<AttributeCatalog>::available(std::move(attributes));
        } else data.security.emplace(SecurityPart::Unknown, Observation<NativeSecurityValue>{});
        bool rejected = false;
        try { fs.set_inventory(object, std::move(data)); }
        catch (const std::length_error&) { rejected = true; }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected && take(held->inspect_storage()).model == StorageModel::LinuxStatx);
    }
}
CK_TEST(simulated_inventory_every_query_can_throw_without_leaking_or_rebinding_handles) {
    FileSystem fs; auto object = inventory_object(fs); auto held = entry(fs);
    const auto before = take(take(held->inspect_basic()).identity);
    for (auto operation : inventory_operations) fs.enqueue(operation, {{}, {}, {}, true});
    int caught = 0;
    try { (void)held->inspect_storage(); } catch (const std::runtime_error&) { ++caught; }
    try { (void)held->read_link_metadata(); } catch (const std::runtime_error&) { ++caught; }
    try { (void)held->inspect_data_channels(); } catch (const std::runtime_error&) { ++caught; }
    try { (void)held->inspect_attributes(); } catch (const std::runtime_error&) { ++caught; }
    try { (void)held->read_attribute(feature()); } catch (const std::runtime_error&) { ++caught; }
    try { (void)held->read_security(SecurityPart::WindowsAccess); } catch (const std::runtime_error&) { ++caught; }
    CK_CHECK(caught == 6 && fs.live_entries() == 1 && fs.live_cursors() == 0);
    CK_CHECK(compare_identity(before, take(take(held->inspect_basic()).identity)) == IdentityComparison::Same);
    for (auto state : inventory_states(*held)) CK_CHECK(state == ObservationState::Available);
    held.reset(); CK_CHECK(fs.live_entries() == 0 && object->id() != 0);
}
CK_TEST(simulated_inventory_catalog_record_limits_are_independent_of_byte_limits) {
    FileSystem fs; auto object = inventory_object(fs); auto data = inventory(fs, object);
    auto channels = take(*data.channels); channels.channels.push_back(channels.channels.front());
    auto attributes = take(*data.attributes); attributes.attributes.push_back(attributes.attributes.front());
    data.channels = Observation<DataChannelCatalog>::available(std::move(channels));
    data.attributes = Observation<AttributeCatalog>::available(std::move(attributes));
    fs.set_inventory(object, std::move(data)); auto held = entry(fs);
    CK_CHECK(held->inspect_data_channels({48, 2}).value() && !held->inspect_data_channels({48, 1}).value());
    CK_CHECK(held->inspect_attributes({16, 2}).value() && !held->inspect_attributes({16, 1}).value());
    CK_CHECK(!held->inspect_data_channels({47, 2}).value() && !held->inspect_attributes({15, 2}).value());
}
CK_TEST(simulated_inventory_windows_reparse_bytes_and_decoded_names_have_distinct_query_and_fixture_budgets) {
    FileSystem fs; auto object = inventory_object(fs); Inventory data;
    WindowsReparseData reparse; reparse.tag = 0x1234; reparse.kind = WindowsReparseKind::OpaqueGuid;
    reparse.raw = {std::byte{0xff}, std::byte{0}};
    reparse.names = WindowsLinkNames{std::u16string{u'x', char16_t{0xd800}}, u"print", 0x80};
    data.link = Observation<LinkMetadata>::available({fs.metadata(object).identity, reparse});
    fs.set_inventory(object, std::move(data)); auto held = entry(fs);
    const auto value = std::get<WindowsReparseData>(take(held->read_link_metadata({2, 1})).payload);
    CK_CHECK(value.raw == reparse.raw && value.names->substitute == reparse.names->substitute && value.names->flags == 0x80);
    CK_CHECK(!held->read_link_metadata({1, 1}).value());
    reparse.names->print.assign(InventoryLimits::hard_native_bytes / 2 + 1, u'x');
    data.link = Observation<LinkMetadata>::available({{}, std::move(reparse)});
    bool refused = false; try { fs.set_inventory(object, std::move(data)); } catch (const std::length_error&) { refused = true; }
    CK_CHECK(refused && held->read_link_metadata({2, 1}).value());
}
CK_TEST(simulated_inventory_oversize_query_keys_are_rejected_before_trace_copy_or_action_consumption) {
    FileSystem fs; (void)inventory_object(fs); auto held = entry(fs);
    fs.enqueue(Operation::ReadAttribute, {denied(), {}, {}, false}); const auto calls = fs.calls().size();
    const auto key = *NativeFeatureIdentifier::from_windows(std::u16string(InventoryLimits::hard_native_bytes / 2 + 1, u'x'));
    CK_CHECK(held->read_attribute(key).state() == ObservationState::Failed && fs.calls().size() == calls);
    CK_CHECK(held->read_attribute(feature()).state() == ObservationState::Denied);
}
