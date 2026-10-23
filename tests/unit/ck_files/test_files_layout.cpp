// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_layout.hpp"
#include "opened_entry.hpp"
#include "simulated_filesystem.hpp"
#include "object_inventory_job.hpp"
#include "cvision/testing/cktest.hpp"
#include <limits>
#include <new>

namespace {
using namespace ck::files;
using Reply = Observation<std::optional<FileDataRange>>;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required data layout observation unavailable");
    return std::move(value).take_value();
}
BasicMetadata basic() {
    BasicMetadata result; result.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
    result.logical_size = Observation<std::uint64_t>::available(100);
    result.identity = Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}});
    result.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(10, 0));
    result.changed = result.modified; return result;
}
Observation<FileDataLayout> collect(const detail::DataRangeQuery& query, std::uint64_t size = 100, InventoryLimits limits = {},
    DataLayoutFamily family = DataLayoutFamily::PosixSeekDataHole, std::stop_token stop = {}) {
    return detail::collect_data_layout(query, family, basic().identity, size, limits, stop);
}
}
CK_TEST(layout_collects_reported_data_ranges_without_inferring_physical_extents_or_nonzero_bytes) {
    int calls = 0;
    const auto result = take(collect([&](std::uint64_t at, std::uint64_t size) {
        ++calls; CK_CHECK(size == 100);
        return at == 0 ? Reply::available(FileDataRange{10, 20}) : at == 30 ? Reply::available(FileDataRange{70, 10}) : Reply::available(std::nullopt);
    }));
    const std::vector<FileDataRange> expected{{10, 20}, {70, 10}};
    CK_CHECK(result.ranges == expected && result.logical_size == 100 && result.family == DataLayoutFamily::PosixSeekDataHole && calls == 3);
}
CK_TEST(layout_preserves_legitimate_whole_file_native_reports_and_never_fabricates_them_on_failure) {
    const auto whole = take(collect([](std::uint64_t, std::uint64_t size) { return Reply::available(FileDataRange{0, size}); }));
    CK_CHECK(whole.ranges.size() == 1 && whole.ranges[0].length == 100);
    const NativeError error{ErrorDomain::Posix, 95, "layout.unsupported"};
    const auto failed = collect([&](std::uint64_t, std::uint64_t) { return Reply::unavailable(ObservationState::Unsupported, error); });
    CK_CHECK(failed.state() == ObservationState::Unsupported && !failed.value() && failed.unavailable_detail()->error == error);
}
CK_TEST(layout_empty_file_still_requires_native_end_evidence_and_keeps_query_failure) {
    int calls = 0;
    const auto empty = collect([&](std::uint64_t at, std::uint64_t size) { ++calls; CK_CHECK(at == 0 && size == 0); return Reply::available(std::nullopt); }, 0);
    CK_CHECK(take(empty).ranges.empty() && calls == 1);
    const auto unknown = collect([](std::uint64_t, std::uint64_t) { return Reply::unavailable(ObservationState::Denied); }, 0);
    CK_CHECK(unknown.state() == ObservationState::Denied && !unknown.value());
}
CK_TEST(layout_preserves_all_failure_states_and_discards_already_collected_prefixes) {
    for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
        ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        int calls = 0; const NativeError error{ErrorDomain::NtStatus, 73, "layout.injected"};
        const auto result = collect([&](std::uint64_t, std::uint64_t) {
            return ++calls == 1 ? Reply::available(FileDataRange{0, 10}) : Reply::unavailable(state, error);
        });
        CK_CHECK(result.state() == state && !result.value() && result.unavailable_detail()->error == error && calls == 2);
    }
}
CK_TEST(layout_record_and_byte_limits_allow_a_final_end_witness_without_allowing_a_prefix) {
    for (bool extra : {false, true}) {
        int calls = 0;
        const auto result = collect([&](std::uint64_t, std::uint64_t) {
            if (++calls == 1) return Reply::available(FileDataRange{0, 10});
            return extra ? Reply::available(FileDataRange{20, 10}) : Reply::available(std::nullopt);
        }, 100, {sizeof(FileDataRange), 1});
        CK_CHECK(calls == 2);
        if (extra) CK_CHECK(result.state() == ObservationState::Unknown && !result.value() && result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
        else CK_CHECK(take(result).ranges.size() == 1);
    }
}
CK_TEST(layout_rejects_invalid_configuration_and_too_small_buffers_before_native_callbacks) {
    int calls = 0; const detail::DataRangeQuery query = [&](std::uint64_t, std::uint64_t) { ++calls; return Reply::available(std::nullopt); };
    for (auto limits : {InventoryLimits{0, 1}, InventoryLimits{16, 0}, InventoryLimits{InventoryLimits::hard_native_bytes + 1, 1}})
        CK_CHECK(collect(query, 100, limits).state() == ObservationState::Failed);
    CK_CHECK(collect(query, 100, {sizeof(FileDataRange) - 1, 1}).state() == ObservationState::Unknown);
    CK_CHECK(collect(query, 100, {}, DataLayoutFamily::Unknown).state() == ObservationState::Failed);
    CK_CHECK(collect({}, 100).state() == ObservationState::Failed && calls == 0);
}
CK_TEST(layout_rejects_zero_overflow_nonprogress_and_outside_request_ranges) {
    for (const auto range : {FileDataRange{0, 0}, FileDataRange{std::numeric_limits<std::uint64_t>::max(), 2}, FileDataRange{100, 1}}) {
        const auto result = collect([&](std::uint64_t, std::uint64_t) { return Reply::available(range); }, 100, {}, DataLayoutFamily::WindowsAllocatedRanges);
        CK_CHECK(result.state() == ObservationState::Failed && !result.value());
    }
    int calls = 0;
    const auto stalled = collect([&](std::uint64_t, std::uint64_t) { ++calls; return Reply::available(FileDataRange{0, 10}); }, 100, {}, DataLayoutFamily::WindowsAllocatedRanges);
    CK_CHECK(stalled.state() == ObservationState::Failed && calls == 2);
}
CK_TEST(layout_clips_native_windows_rounding_but_rejects_backward_posix_reports) {
    for (auto family : {DataLayoutFamily::WindowsAllocatedRanges, DataLayoutFamily::PosixSeekDataHole}) {
        int calls = 0;
        const auto result = collect([&](std::uint64_t, std::uint64_t) {
            return ++calls == 1 ? Reply::available(FileDataRange{0, 10}) : Reply::available(FileDataRange{0, 200});
        }, 100, {}, family);
        if (family == DataLayoutFamily::WindowsAllocatedRanges) {
            const std::vector<FileDataRange> expected{{0, 10}, {10, 90}}; CK_CHECK(take(result).ranges == expected);
        } else CK_CHECK(result.state() == ObservationState::Failed && !result.value());
    }
    const auto growth = collect([](std::uint64_t, std::uint64_t) { return Reply::available(FileDataRange{100, 1}); });
    CK_CHECK(growth.state() == ObservationState::Changed);
}
CK_TEST(layout_cancellation_before_and_after_callbacks_discards_private_ranges) {
    std::stop_source stopped; stopped.request_stop(); int calls = 0;
    CK_CHECK(collect([&](std::uint64_t, std::uint64_t) { ++calls; return Reply::available(std::nullopt); }, 100, {}, DataLayoutFamily::PosixSeekDataHole, stopped.get_token()).state() == ObservationState::Unknown);
    CK_CHECK(calls == 0);
    std::stop_source stop;
    const auto result = collect([&](std::uint64_t, std::uint64_t) {
        if (++calls == 2) stop.request_stop();
        return Reply::available(FileDataRange{static_cast<std::uint64_t>(calls * 10), 5});
    }, 100, {}, DataLayoutFamily::PosixSeekDataHole, stop.get_token());
    CK_CHECK(result.state() == ObservationState::Unknown && result.unavailable_detail()->error.operation == "layout.cancelled" && calls == 2);
}
CK_TEST(layout_callback_exceptions_are_not_translated_into_success_or_an_empty_layout) {
    int calls = 0; bool caught = false;
    try { (void)collect([&](std::uint64_t, std::uint64_t) -> Reply {
        if (++calls == 1) return Reply::available(FileDataRange{0, 10});
        throw std::bad_alloc{};
    }); } catch (const std::bad_alloc&) { caught = true; }
    CK_CHECK(caught && calls == 2);
}
CK_TEST(layout_metadata_checks_qualify_identity_kind_size_and_available_change_times_only) {
    const auto before = basic(); auto after = before;
    CK_CHECK(take(detail::data_layout_metadata_unchanged(before, after)));
    for (int mode = 0; mode < 5; ++mode) {
        after = before;
        if (mode == 0) after.logical_size = Observation<std::uint64_t>::available(101);
        if (mode == 1) after.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        if (mode == 2) { auto identity = take(after.identity); identity.object = NativeIdentifier::from_u64(3); after.identity = Observation<ObjectIdentity>::available(identity); }
        if (mode == 3) after.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(11, 0));
        if (mode == 4) after.changed = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(11, 0));
        CK_CHECK(detail::data_layout_metadata_unchanged(before, after).state() == ObservationState::Changed);
    }
    after = before; after.identity = {}; CK_CHECK(detail::data_layout_metadata_unchanged(before, after).state() == ObservationState::Unknown);
    after = before; auto foreign = take(after.identity); foreign.scope = AdapterScope{}; after.identity = Observation<ObjectIdentity>::available(foreign);
    CK_CHECK(detail::data_layout_metadata_unchanged(before, after).state() == ObservationState::Unknown);
}
CK_TEST(layout_access_guard_refuses_special_objects_and_identified_provider_recall_without_elevation) {
    auto metadata = basic();
    for (auto kind : {ObjectKind::Directory, ObjectKind::SymbolicLink, ObjectKind::Junction, ObjectKind::Fifo, ObjectKind::CharacterDevice}) {
        metadata.kind = Observation<ObjectKind>::available(kind);
        CK_CHECK(detail::data_layout_access_guard(metadata, StorageModel::LinuxStatx).state() == ObservationState::Unsupported);
    }
    metadata = basic(); CK_CHECK(take(detail::data_layout_access_guard(metadata, StorageModel::LinuxStatx)));
    metadata.windows_attributes = Observation<std::uint32_t>::available(0);
    CK_CHECK(take(detail::data_layout_access_guard(metadata, StorageModel::WindowsHandleAttributes)));
    metadata.windows_attributes = Observation<std::uint32_t>::available(0x1000);
    CK_CHECK(detail::data_layout_access_guard(metadata, StorageModel::WindowsHandleAttributes).unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::ProviderReviewRequired));
    metadata.bsd_flags = Observation<std::uint32_t>::available(0x40000000);
    CK_CHECK(detail::data_layout_access_guard(metadata, StorageModel::DarwinStatFlags).state() == ObservationState::Unknown);
    CK_CHECK(detail::data_layout_access_guard(metadata, StorageModel::Unknown).state() == ObservationState::Failed);
}
CK_TEST(layout_collector_rejects_malformed_layout_envelopes_and_overlapping_ranges) {
    using FS = ck::testing::SimulatedFileSystem;
    for (int mode = 0; mode < 5; ++mode) {
        FS fs; auto root = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
#ifdef _WIN32
        const std::filesystem::path path = L"V:/simulated-layout";
#else
        const std::filesystem::path path = "/simulated-layout";
#endif
        const auto name = *NativeName::from_posix("file"); fs.bind(path, root); fs.put(root, name, file);
        FileDataLayout layout{DataLayoutFamily::PosixSeekDataHole, fs.metadata(file).identity, 100, {{10, 20}}};
        if (mode == 0) layout.family = DataLayoutFamily::Unknown;
        if (mode == 1) layout.ranges[0].length = 0;
        if (mode == 2) layout.ranges[0] = {90, 20};
        if (mode == 3) layout.ranges.push_back({20, 10});
        if (mode == 4) layout.ranges.push_back({5, 1});
        ck::testing::SimulatedInventory inventory; inventory.layout = Observation<FileDataLayout>::available(layout); fs.set_inventory(file, std::move(inventory));
        auto opened = take(take(fs.open_directory(path))->open_child(name, EntryAccess::MetadataAndLayout));
        ObjectInventoryJob job(std::move(opened), {}, fs.clock());
        CK_CHECK(job.advance() == ObjectInventoryJobState::Failed && !job.completed().value() && fs.live_entries() == 0);
    }
}
