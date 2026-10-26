// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_planning_service.hpp"

#include "simulated_filesystem.hpp"
#include <cvision/testing/cktest.hpp>

#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace ck::files;
using namespace ck::vision;
using FS = ck::testing::SimulatedFileSystem;

template <class T>
T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("planning service fixture unavailable");
    return std::move(value).take_value();
}

NativeName name(const char* text) { return *NativeName::from_posix(text); }

FrozenInventory::Pointer inventory() {
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    auto source = NativeAbsolutePath::from_posix("/source/item");
    if (!source.value() || !builder.add({std::move(source).take_value(),
            {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                NativeIdentifier::from_u64(1), {}},
            ObjectKind::RegularFile, Observation<std::uint64_t>::available(1), 1,
            LinkTraversalEvidence::NotLink, {}, {}}) ||
        !builder.seal())
        throw std::runtime_error("planning service inventory unavailable");
    return take(builder.completed());
}

ObjectInventoryData object_inventory() {
    ObjectInventoryData result;
    const auto unsupported = []<class T>() {
        return Observation<T>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "fixture.unsupported"});
    };
    result.before = unsupported.template operator()<BasicMetadata>();
    result.after = unsupported.template operator()<BasicMetadata>();
    result.storage = unsupported.template operator()<StorageInfo>();
    result.integrity = unsupported.template operator()<FileIntegrityInfo>();
    result.inode_flags = unsupported.template operator()<FileInodeFlags>();
    result.layout = unsupported.template operator()<FileDataLayout>();
    result.link = unsupported.template operator()<LinkMetadata>();
    result.link_limits = unsupported.template operator()<FileLinkLimits>();
    result.channels = unsupported.template operator()<DataChannelCatalog>();
    result.attributes = unsupported.template operator()<AttributeCatalog>();
    for (auto& security : result.security)
        security = unsupported.template operator()<NativeSecurityValue>();
    return result;
}

struct Fixture final {
    FS fs;
    FS::Pointer destination = fs.make_node(ObjectKind::Directory);
    std::shared_ptr<const DirectoryCursor> cursor;

    Fixture() {
        fs.bind("/planning-service", destination);
        cursor = std::shared_ptr<const DirectoryCursor>(take(fs.open_directory("/planning-service")));
    }

    std::shared_ptr<const CompletedDestinationBatch> batch() {
        DestinationBatchJob job(cursor, {name("missing")}, NameEncoding::PosixBytes, {}, fs.clock());
        for (int step = 0; step < 200; ++step)
            if (job.advance() == DestinationBatchState::Complete)
                return take(job.completed());
        throw std::runtime_error("planning service batch exceeded step bound");
    }
};

FilesOperationPlanningRequest request(Fixture& fixture) {
    auto plan_inventory = inventory();
    const std::array objects{object_inventory()};
    BasicMetadata source_metadata;
    source_metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
    const std::array source_target{SourceTargetInspection{
        Observation<BasicMetadata>::available(source_metadata), {},
        Observation<IdentityComparison>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable),
                "source_target.no_existing_target"})}};
    const std::array ancestry{Observation<AncestorWalkWitness>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "fixture.not_needed"})};
    const std::array capabilities{DestinationCapabilityEvidence{
        VolumeFeature::SparseFiles, Observation<FeatureAdvertisement>::available(FeatureAdvertisement::Advertised)}};
    FilesOperationPlanningRequest result;
    result.operation = FileOperation::Copy;
    result.inventory = std::move(plan_inventory);
    result.source_target.assign(source_target.begin(), source_target.end());
    result.ancestry.assign(ancestry.begin(), ancestry.end());
    result.source_objects.assign(objects.begin(), objects.end());
    result.destination_batch = fixture.batch();
    result.destination_capabilities.assign(capabilities.begin(), capabilities.end());
    return result;
}

FilesOperationNativePlanningRequest native_request(Fixture& fixture) {
    auto source_parent = fixture.fs.make_node(ObjectKind::Directory);
    auto source_file = fixture.fs.make_node(ObjectKind::RegularFile);
    fixture.fs.bind("/planning-service-source", source_parent);
    fixture.fs.put(source_parent, name("file"), source_file);
    fixture.fs.metadata(source_file).logical_size = Observation<std::uint64_t>::available(4);
    fixture.fs.metadata(source_file).mount_id = Observation<std::uint64_t>::available(1);

    std::shared_ptr<const DirectoryCursor> parent(take(fixture.fs.open_directory("/planning-service-source")));
    auto source = take(HeldChild::observe(parent, name("file")));

    FilesOperationNativePlanningRequest result;
    result.evidence.direct_sources.push_back(std::move(source));
    result.evidence.direct_source_paths.push_back(take(NativeAbsolutePath::from_posix("/planning-service-source/file")));
    result.evidence.destination = fixture.cursor;
    result.evidence.destination_names = {name("file")};
    result.evidence.clock = fixture.fs.clock();
    result.evidence.limits.max_items = 1;
    result.evidence.limits.destination_batch.rows = 1;
    result.evidence.limits.source_target.rows = 1;
    return result;
}

struct Wake final {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t count = 0;
    std::thread::id callback_thread;

    void notify() {
        std::lock_guard lock(mutex);
        ++count;
        callback_thread = std::this_thread::get_id();
        condition.notify_all();
    }

    FilesOperationPlanningReply reply(FilesOperationPlanningService& service) {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return count != 0; }))
            throw std::runtime_error("planning service reply timeout");
        auto value = service.take();
        if (!value)
            throw std::runtime_error("planning service wake without reply");
        return std::move(*value);
    }
};
} // namespace

CK_TEST(operation_planning_service_publishes_plan_and_review_from_worker_values) {
    Fixture fixture;
    Wake wake;
    FilesOperationPlanningService service([&] { wake.notify(); });
    auto input = request(fixture);
    const auto submitted = service.request(std::move(input));
    CK_CHECK(submitted.state == FilesOperationPlanningSubmission::Accepted && submitted.ticket.valid());
    const auto reply = wake.reply(service);
    CK_CHECK(reply.request == submitted.ticket);
    CK_CHECK(reply.state == FilesOperationPlanningReplyState::Ready);
    CK_CHECK(reply.plan);
    CK_CHECK(reply.review);
    if (reply.plan && reply.review)
        CK_CHECK(reply.review->plan() == reply.plan && reply.plan->items().size() == 1 && reply.plan->conflicts().empty());
    CK_CHECK(wake.callback_thread != std::this_thread::get_id());
    service.join();
    CK_CHECK(!service.running());
}

CK_TEST(operation_planning_service_composes_native_evidence_before_publishing_plan_and_review) {
    Fixture fixture;
    Wake wake;
    FilesOperationPlanningService service([&] { wake.notify(); });
    auto input = native_request(fixture);
    const auto submitted = service.request(std::move(input));
    CK_CHECK(submitted.state == FilesOperationPlanningSubmission::Accepted && submitted.ticket.valid());
    const auto reply = wake.reply(service);
    CK_CHECK(reply.request == submitted.ticket && reply.state == FilesOperationPlanningReplyState::Ready);
    CK_CHECK(reply.plan && reply.review && reply.plan->items().size() == 1 && reply.plan->conflicts().empty());
    service.join();
}

CK_TEST(operation_planning_service_rejects_shape_invalid_requests_before_worker_submission) {
    Wake wake;
    FilesOperationPlanningService service([&] { wake.notify(); });
    FilesOperationPlanningRequest invalid;
    CK_CHECK(service.request(std::move(invalid)).state == FilesOperationPlanningSubmission::Invalid);
    CK_CHECK(!service.working());
    CK_CHECK(wake.count == 0 && service.fault() == FilesPaneWorkerFault::None);
    service.join();
}

CK_TEST(operation_planning_service_cancellation_is_a_terminal_non_plan_reply) {
    Fixture fixture;
    Wake wake;
    FilesOperationPlanningService service([&] { wake.notify(); });
    const auto submitted = service.request(request(fixture));
    CK_CHECK(submitted.state == FilesOperationPlanningSubmission::Accepted);
    CK_CHECK(service.cancel(submitted.ticket));
    const auto reply = wake.reply(service);
    CK_CHECK(reply.request == submitted.ticket && reply.state == FilesOperationPlanningReplyState::Cancelled &&
        !reply.plan && !reply.review);
    service.join();
}
