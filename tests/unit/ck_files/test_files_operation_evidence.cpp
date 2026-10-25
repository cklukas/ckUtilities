// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_evidence_job.hpp"

#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"

#include <stdexcept>
#include <string>

namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;

template <class T>
T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("operation evidence fixture unavailable");
    return std::move(value).take_value();
}

NativeName name(const char* value) { return *NativeName::from_posix(value); }

NativeAbsolutePath path(const char* value) { return take(NativeAbsolutePath::from_posix(value)); }

OperationEvidenceJob::Result finish(OperationEvidenceJob& job) {
    for (int attempt = 0; attempt != 400; ++attempt) {
        const auto state = job.advance();
        if (state == OperationEvidenceJobState::Complete) return take(job.completed());
        if (state == OperationEvidenceJobState::Failed)
            throw std::runtime_error("operation evidence job failed " +
                (job.failure() ? job.failure()->error.operation + " code=" +
                    std::to_string(job.failure()->error.code) : "missing"));
    }
    throw std::runtime_error("operation evidence job exceeded step bound");
}
} // namespace

CK_TEST(operation_evidence_job_composes_recursive_objects_destination_and_topology_evidence) {
    FS fs;
    auto source_parent = fs.make_node(ObjectKind::Directory);
    auto source_root = fs.make_node(ObjectKind::Directory);
    auto source_file = fs.make_node(ObjectKind::RegularFile);
    auto destination = fs.make_node(ObjectKind::Directory);
    fs.bind("/operation-evidence-source", source_parent);
    fs.bind("/operation-evidence-destination", destination);
    fs.put(source_parent, name("root"), source_root);
    fs.put(source_root, name("file"), source_file);
    fs.metadata(source_root).logical_size = Observation<std::uint64_t>::available(0);
    fs.metadata(source_file).logical_size = Observation<std::uint64_t>::available(4);
    fs.metadata(source_root).mount_id = Observation<std::uint64_t>::available(1);
    fs.metadata(source_file).mount_id = Observation<std::uint64_t>::available(1);

    std::shared_ptr<const DirectoryCursor> source_parent_cursor(take(fs.open_directory("/operation-evidence-source")));
    auto root_source = take(HeldChild::observe(source_parent_cursor, name("root")));
    auto source_cursor = std::shared_ptr<DirectoryCursor>(take(source_parent_cursor->open_directory_child(name("root"))));
    std::shared_ptr<const DirectoryCursor> destination_cursor(take(fs.open_directory("/operation-evidence-destination")));

    OperationEvidenceJobRequest request;
    request.root_source = std::move(root_source);
    request.source_root = std::move(source_cursor);
    request.source_path = path("/operation-evidence-source/root");
    request.destination = std::move(destination_cursor);
    request.destination_names = {name("root"), name("file")};
    request.clock = fs.clock();
    request.limits.max_items = 2;
    request.limits.destination_batch.rows = 2;
    request.limits.source_target.rows = 2;

    OperationEvidenceJob job(std::move(request));
    const auto result = finish(job);
    CK_CHECK(result->inventory() && result->inventory()->entries().size() == 2);
    CK_CHECK(result->source_objects().size() == 2);
    CK_CHECK(result->destination_batch() && result->destination_batch()->entries().size() == 2);
    CK_CHECK(result->source_target().size() == 2 && result->ancestry().size() == 2);
    CK_CHECK(result->destination_capabilities().size() == destination_advertised_features.size());
    CK_CHECK(job.advance() == OperationEvidenceJobState::Complete);
}

CK_TEST(operation_evidence_job_rejects_destination_shape_before_native_batch_work) {
    FS fs;
    auto source_parent = fs.make_node(ObjectKind::Directory);
    auto source_root = fs.make_node(ObjectKind::Directory);
    auto destination = fs.make_node(ObjectKind::Directory);
    fs.bind("/operation-evidence-shape-source", source_parent);
    fs.bind("/operation-evidence-shape-destination", destination);
    fs.put(source_parent, name("root"), source_root);
    fs.metadata(source_root).logical_size = Observation<std::uint64_t>::available(0);
    fs.metadata(source_root).mount_id = Observation<std::uint64_t>::available(1);

    std::shared_ptr<const DirectoryCursor> source_parent_cursor(take(fs.open_directory("/operation-evidence-shape-source")));
    auto root_source = take(HeldChild::observe(source_parent_cursor, name("root")));
    auto source_cursor = std::shared_ptr<DirectoryCursor>(take(source_parent_cursor->open_directory_child(name("root"))));
    std::shared_ptr<const DirectoryCursor> destination_cursor(take(fs.open_directory("/operation-evidence-shape-destination")));

    OperationEvidenceJobRequest request;
    request.root_source = std::move(root_source);
    request.source_root = std::move(source_cursor);
    request.source_path = path("/operation-evidence-shape-source/root");
    request.destination = std::move(destination_cursor);
    request.destination_names = {};
    request.clock = fs.clock();
    request.limits.max_items = 1;
    request.limits.destination_batch.rows = 1;
    request.limits.source_target.rows = 1;

    OperationEvidenceJob job(std::move(request));
    CK_CHECK(job.advance() == OperationEvidenceJobState::Failed);
    CK_CHECK(job.failure() && job.failure()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));
    CK_CHECK(!job.completed().value());
}
