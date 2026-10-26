// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_conflict_projection.hpp"

#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;

template <class T>
T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("destination conflict fixture unavailable");
    return std::move(value).take_value();
}

NativeName name(const char* text) { return *NativeName::from_posix(text); }

std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/destination-conflict-projection";
#else
    return "/destination-conflict-projection";
#endif
}

DestinationBatchJob::Result finish(DestinationBatchJob& job) {
    for (int step = 0; step < 200; ++step)
        if (job.advance() == DestinationBatchState::Complete) return take(job.completed());
    throw std::runtime_error("destination conflict fixture exceeded step bound");
}

FrozenInventory::Pointer inventory(std::initializer_list<ObjectKind> kinds) {
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    std::size_t index = 0;
    for (const auto kind : kinds) {
        const auto text = std::string{"/source/item-"} + std::to_string(index);
        auto source = NativeAbsolutePath::from_posix(text);
        if (!source.value()) throw std::runtime_error("destination conflict path unavailable");
        if (!builder.add({std::move(source).take_value(),
            {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                NativeIdentifier::from_u64(index + 1), {}}, kind,
            Observation<std::uint64_t>::available(1), 1, LinkTraversalEvidence::NotLink, {}, {}}))
            throw std::runtime_error("destination conflict inventory unavailable");
        ++index;
    }
    if (index == 0 || !builder.seal())
        throw std::runtime_error("destination conflict inventory unavailable");
    return take(builder.completed());
}

struct Fixture final {
    FS fs;
    FS::Pointer destination = fs.make_node(ObjectKind::Directory);
    std::shared_ptr<const DirectoryCursor> cursor;

    Fixture() {
        fs.bind(path(), destination);
        cursor = std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path())));
    }

    DestinationBatchJob::Result batch(std::vector<NativeName> names) {
        DestinationBatchJob job(cursor, std::move(names), NameEncoding::PosixBytes, {}, fs.clock());
        return finish(job);
    }

    void put(const char* text, ObjectKind kind) { fs.put(destination, name(text), fs.make_node(kind)); }
};
} // namespace

CK_TEST(destination_conflict_projection_distinguishes_existing_file_directory_and_type_mismatch) {
    Fixture f;
    f.put("file", ObjectKind::RegularFile);
    f.put("dir", ObjectKind::Directory);
    const auto batch = f.batch({name("file"), name("dir"), name("missing")});
    const auto conflicts = DestinationConflictProjection::from_batch(*inventory({ObjectKind::RegularFile,
        ObjectKind::Directory, ObjectKind::RegularFile}), *batch);
    CK_CHECK(conflicts.value() && conflicts.value()->size() == 2);
    CK_CHECK((*conflicts.value())[0].kind == DestinationConflictKind::ExistingFile &&
        (*conflicts.value())[1].kind == DestinationConflictKind::ExistingDirectory);

    const auto mismatch = DestinationConflictProjection::from_batch(*inventory({ObjectKind::Directory}),
        *f.batch({name("file")}));
    CK_CHECK(mismatch.value() && mismatch.value()->size() == 1 &&
        mismatch.value()->front().kind == DestinationConflictKind::TypeMismatch);
}

CK_TEST(destination_conflict_projection_keeps_exact_collisions_and_unreadable_targets_conservative) {
    Fixture f;
    f.put("file", ObjectKind::RegularFile);
    const auto duplicate = DestinationConflictProjection::from_batch(*inventory({ObjectKind::RegularFile,
        ObjectKind::RegularFile}), *f.batch({name("file"), name("file")}));
    CK_CHECK(duplicate.value() && duplicate.value()->size() == 2 &&
        (*duplicate.value())[0].kind == DestinationConflictKind::CaseCollision &&
        (*duplicate.value())[1].kind == DestinationConflictKind::CaseCollision);

    f.fs.enqueue(FS::Operation::LookupChild, {UnavailableObservation{ObservationState::Denied,
        {ErrorDomain::Posix, 13, "fixture.lookup_denied"}}, {}, {}, false});
    const auto unknown = DestinationConflictProjection::from_batch(*inventory({ObjectKind::RegularFile}),
        *f.batch({name("file")}));
    CK_CHECK(unknown.value() && unknown.value()->size() == 1 &&
        unknown.value()->front().kind == DestinationConflictKind::Unknown);
}

CK_TEST(destination_conflict_projection_rejects_shape_and_limits_before_projection) {
    Fixture f;
    DestinationConflictProjectionLimits limits;
    limits.max_conflicts = 0;
    const auto invalid_limit = DestinationConflictProjection::from_batch(*inventory({ObjectKind::RegularFile}),
        *f.batch({name("missing")}), limits);
    CK_CHECK(invalid_limit.state() == ObservationState::Failed &&
        invalid_limit.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));

    const auto wrong_shape = DestinationConflictProjection::from_batch(*inventory({ObjectKind::RegularFile,
        ObjectKind::RegularFile}), *f.batch({name("missing")}));
    CK_CHECK(wrong_shape.state() == ObservationState::Failed &&
        wrong_shape.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));
}
