// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "location_relation.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
namespace {
using namespace ck::files;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("location relation fixture unavailable");
    return std::move(value).take_value();
}
using Subject = detail::LocationSubjectBookends;
struct Fixture {
    AdapterScope scope;
    Subject directory, object;
    Observation<LocationInfo> first,last;
    Fixture() {
        directory = subject(1,ObjectKind::Directory); object = subject(2,ObjectKind::RegularFile);
        LocationInfo location; location.family = LocationQueryFamily::LinuxDescriptor; location.directory = directory.anchor;
        first = Observation<LocationInfo>::available(location); last = first;
    }
    Subject subject(std::uint64_t number, ObjectKind kind) const {
        BasicMetadata m; m.identity = Observation<ObjectIdentity>::available({scope,IdentityScheme::PosixDeviceInode,NativeIdentifier::from_u64(7),NativeIdentifier::from_u64(number),{}});
        m.kind = Observation<ObjectKind>::available(kind); m.mount_id = Observation<std::uint64_t>::available(17);
        return {m.identity,Observation<BasicMetadata>::available(m),Observation<BasicMetadata>::available(m)};
    }
    LocationRelationData assess() const { return detail::assess_location_bookends(directory,object,first,last); }
};
void mount(Subject& value, std::uint64_t id, bool before = true, bool after = true) {
    if (before) { auto m = take(value.before); m.mount_id = Observation<std::uint64_t>::available(id); value.before = Observation<BasicMetadata>::available(m); }
    if (after) { auto m = take(value.after); m.mount_id = Observation<std::uint64_t>::available(id); value.after = Observation<BasicMetadata>::available(m); }
}
void identity(Subject& value, ObjectIdentity id) {
    value.anchor = Observation<ObjectIdentity>::available(id);
    for (auto* endpoint : {&value.before,&value.after}) { auto m = take(*endpoint); m.identity = value.anchor; *endpoint = Observation<BasicMetadata>::available(m); }
}
constexpr std::array states{ObservationState::Unsupported,ObservationState::Unknown,ObservationState::Denied,ObservationState::Gone,ObservationState::Changed,ObservationState::Failed};
NativeError injected() { return {ErrorDomain::Posix,13,"relation.injected"}; }
}
CK_TEST(location_relation_keeps_volume_identifier_and_linux_mount_comparisons_independent) {
    for (const bool other_volume : {false,true}) for (const bool other_mount : {false,true}) {
        Fixture f;
        if (other_volume) { auto id = take(f.object.anchor); id.volume = NativeIdentifier::from_u64(8); identity(f.object,id); }
        if (other_mount) mount(f.object,18);
        const auto result = f.assess();
        CK_CHECK(result.directory.value() && result.object.value());
        CK_CHECK(take(result.volumes) == (other_volume ? NativeVolumeRelation::DifferentReportedIdentifiers : NativeVolumeRelation::SameReportedIdentifier));
        CK_CHECK(take(result.linux_mounts) == (other_mount ? LinuxMountRelation::DifferentReportedIds : LinuxMountRelation::SameReportedId));
    }
}
CK_TEST(location_relation_preserves_all_subject_failures_without_equating_unknown_owners) {
    for (auto state : states) for (bool child : {false,true}) for (bool after : {false,true}) for (int field = 0; field < 4; ++field) {
        Fixture f; auto& subject = child ? f.object : f.directory; auto& endpoint = after ? subject.after : subject.before;
        if (field == 0) subject.anchor = Observation<ObjectIdentity>::unavailable(state,injected());
        else if (field == 1) endpoint = Observation<BasicMetadata>::unavailable(state,injected());
        else { auto m = take(endpoint);
            if (field == 2) m.identity = Observation<ObjectIdentity>::unavailable(state,injected());
            else m.kind = Observation<ObjectKind>::unavailable(state,injected());
            endpoint = Observation<BasicMetadata>::available(m);
        }
        const auto result = f.assess();
        CK_CHECK(result.volumes.state() == state && result.linux_mounts.state() == state);
        CK_CHECK(result.volumes.unavailable_detail()->error == injected() && result.linux_mounts.unavailable_detail()->error == injected());
    }
}
CK_TEST(location_relation_rejects_changed_kind_or_identity_and_unqualified_bookends) {
    for (bool child : {false,true}) for (int mode = 0; mode < 6; ++mode) {
        Fixture f; auto& subject = child ? f.object : f.directory; auto m = take(subject.after); auto id = take(m.identity);
        if (mode == 0) id.object = NativeIdentifier::from_u64(99);
        if (mode == 1) id.scope = AdapterScope{};
        if (mode == 2) id.object = {};
        if (mode == 3) id.scheme = static_cast<IdentityScheme>(255);
        m.identity = Observation<ObjectIdentity>::available(id);
        if (mode == 4) m.kind = Observation<ObjectKind>::available(child ? ObjectKind::Directory : ObjectKind::RegularFile);
        if (mode == 5) m.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        subject.after = Observation<BasicMetadata>::available(m);
        const auto result = f.assess(); const auto state = mode == 0 || mode == 4 ? ObservationState::Changed : mode == 5 ? ObservationState::Failed : ObservationState::Unknown;
        CK_CHECK(result.volumes.state() == state && result.linux_mounts.state() == state);
    }
}
CK_TEST(location_relation_does_not_compare_volume_or_mount_numbers_across_identity_namespaces) {
    for (bool scheme : {false,true}) {
        Fixture f; auto id = take(f.object.anchor);
        if (scheme) id.scheme = IdentityScheme::WindowsFileId128; else id.scope = AdapterScope{};
        identity(f.object,id); const auto result = f.assess();
        CK_CHECK(result.directory.value() && result.object.value());
        CK_CHECK(result.volumes.state() == ObservationState::Unknown && result.linux_mounts.state() == ObservationState::Unknown);
    }
}
CK_TEST(location_relation_mount_refusals_do_not_become_same_mount_from_equal_volume_ids) {
    for (auto state : states) for (bool child : {false,true}) for (bool after : {false,true}) {
        Fixture f; auto& endpoint = child ? (after ? f.object.after : f.object.before) : (after ? f.directory.after : f.directory.before);
        auto m = take(endpoint); m.mount_id = Observation<std::uint64_t>::unavailable(state,injected()); endpoint = Observation<BasicMetadata>::available(m);
        const auto result = f.assess(); CK_CHECK(take(result.volumes) == NativeVolumeRelation::SameReportedIdentifier);
        CK_CHECK(result.linux_mounts.state() == state && result.linux_mounts.unavailable_detail()->error == injected());
    }
    for (bool child : {false,true}) {
        Fixture f; mount(child ? f.object : f.directory,99,false,true); const auto result = f.assess();
        CK_CHECK(result.volumes.value() && result.linux_mounts.state() == ObservationState::Changed);
    }
}
CK_TEST(location_relation_requires_qualified_linux_query_family_before_interpreting_mount_ids) {
    for (int mode = 0; mode < 8; ++mode) {
        Fixture f; auto a = take(f.first), b = a;
        if (mode == 0) a.family = b.family = LocationQueryFamily::DarwinDescriptor;
        if (mode == 1) a.family = b.family = LocationQueryFamily::WindowsHandle;
        if (mode == 2) a.family = b.family = LocationQueryFamily::Unknown;
        if (mode == 3) a.family = b.family = static_cast<LocationQueryFamily>(255);
        if (mode == 4) b.family = LocationQueryFamily::DarwinDescriptor;
        if (mode == 5) b.directory = f.object.anchor;
        if (mode == 6) b.directory = Observation<ObjectIdentity>::unavailable(ObservationState::Denied,injected());
        f.first = Observation<LocationInfo>::available(a); f.last = mode == 7 ? Observation<LocationInfo>::unavailable(ObservationState::Gone,injected()) : Observation<LocationInfo>::available(b);
        const auto result = f.assess(); CK_CHECK(result.volumes.value());
        const auto expected = mode < 2 ? ObservationState::Unsupported : mode == 2 ? ObservationState::Unknown : mode == 3 ? ObservationState::Failed : mode < 6 ? ObservationState::Changed : mode == 6 ? ObservationState::Denied : ObservationState::Gone;
        CK_CHECK(result.linux_mounts.state() == expected);
    }
}
CK_TEST(location_relation_supports_native_windows_volume_identifiers_but_never_invents_windows_mount_ids) {
    for (auto scheme : {IdentityScheme::WindowsFileId128,IdentityScheme::WindowsFileIndex64}) {
        Fixture f; auto a = take(f.directory.anchor), b = take(f.object.anchor); a.scheme = b.scheme = scheme;
        identity(f.directory,a); identity(f.object,b);
        LocationInfo location; location.family = LocationQueryFamily::WindowsHandle; location.directory = f.directory.anchor;
        f.first = Observation<LocationInfo>::available(location); f.last = f.first;
        const auto result = f.assess(); CK_CHECK(take(result.volumes) == NativeVolumeRelation::SameReportedIdentifier);
        CK_CHECK(result.linux_mounts.state() == ObservationState::Unsupported);
        location.family = LocationQueryFamily::LinuxDescriptor; f.first = Observation<LocationInfo>::available(location); f.last = f.first;
        CK_CHECK(f.assess().linux_mounts.state() == ObservationState::Unknown);
    }
}
CK_TEST(location_relation_inspects_special_objects_as_themselves_without_granting_traversal_or_following_links) {
    for (auto kind : {ObjectKind::RegularFile,ObjectKind::Directory,ObjectKind::SymbolicLink,ObjectKind::Junction,ObjectKind::ReparsePoint,
        ObjectKind::Fifo,ObjectKind::Socket,ObjectKind::CharacterDevice,ObjectKind::BlockDevice,ObjectKind::Other}) {
        Fixture f; f.object = f.subject(3,kind); CK_CHECK(f.assess().linux_mounts.value());
    }
    Fixture f; f.directory = f.subject(1,ObjectKind::RegularFile);
    CK_CHECK(f.assess().volumes.state() == ObservationState::Failed);
    CK_CHECK(ObjectLocationRelation::inspect({},{}).state() == ObservationState::Failed);
}

namespace {
struct Script {
    Fixture fixture; int calls = 0, cursors = 0, entries = 0;
};
class Cursor final : public DirectoryCursor {
public:
    explicit Cursor(std::shared_ptr<Script> script) : DirectoryCursor(script->fixture.directory.anchor), script_(std::move(script)) { ++script_->cursors; }
    ~Cursor() override { --script_->cursors; }
    Observation<BasicMetadata> inspect_basic() const override { ++script_->calls; return script_->fixture.directory.before; }
    Observation<LocationInfo> inspect_location() const override { ++script_->calls; return script_->fixture.first; }
private:
    Observation<DirectoryStep> next_step() override { throw std::runtime_error("location comparison enumerated"); }
    std::shared_ptr<Script> script_;
};
class Entry final : public OpenedEntry {
public:
    explicit Entry(std::shared_ptr<Script> script) : script_(std::move(script)) { ++script_->entries; }
    ~Entry() override { --script_->entries; }
    Observation<BasicMetadata> inspect_basic() const override { ++script_->calls; return script_->fixture.object.before; }
private: std::shared_ptr<Script> script_;
};
}
CK_TEST(location_relation_seals_existing_inventory_owners_without_native_calls_and_retires_them_together) {
    auto script = std::make_shared<Script>();
    DirectoryInventoryJob directory(std::make_shared<Cursor>(script)); ObjectInventoryJob object(std::make_unique<Entry>(script));
    for (int i = 0; i < 100 && directory.state() != DirectoryInventoryState::Complete; ++i) (void)directory.advance();
    for (int i = 0; i < 100 && object.state() != ObjectInventoryJobState::Complete; ++i) (void)object.advance();
    const auto calls = script->calls;
    auto result = take(ObjectLocationRelation::inspect(take(directory.completed()),take(object.completed())));
    CK_CHECK(script->calls == calls && result->data().volumes.value() && result->data().linux_mounts.value());
    CK_CHECK(directory.abandon() && object.abandon() && script->cursors == 1 && script->entries == 1);
    CK_CHECK(!ObjectLocationRelation::inspect({},result->object()).value() && !ObjectLocationRelation::inspect(result->directory(),{}).value());
    result.reset(); CK_CHECK(script->cursors == 0 && script->entries == 0);
}
