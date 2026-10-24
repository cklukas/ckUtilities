// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_inventory_job.hpp"
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <functional>
#include <new>
#include <stdexcept>
namespace {
using namespace ck::files;
using State = DirectoryInventoryState;
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("directory inventory fixture unavailable");
    return std::move(result).take_value();
}
struct Script {
    std::shared_ptr<ck::testing::ManualMonotonicClock> clock = std::make_shared<ck::testing::ManualMonotonicClock>();
    ObjectIdentity id{AdapterScope{}, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(7), NativeIdentifier::from_u64(8), {}};
    Observation<ObjectIdentity> anchor = Observation<ObjectIdentity>::available(id);
    BasicMetadata before, after;
    LocationInfo first_location, last_location;
    Observation<DirectoryFscryptPolicy> policy = Observation<DirectoryFscryptPolicy>::available({id, FscryptPolicyAbsent{{ErrorDomain::Posix, 61, "absent"}}});
    Observation<DirectoryFscryptKeyStatus> key = Observation<DirectoryFscryptKeyStatus>::unavailable(ObservationState::Unsupported, {ErrorDomain::Adapter, 3, "key"});
    std::array<std::optional<UnavailableObservation>, 10> failures;
    std::function<void(std::size_t)> hook;
    std::vector<int> calls;
    int basics = 0, locations = 0, live = 0;
    bool wrong_access = false, wrong_family = false;
    Script() {
        before.identity = anchor; before.kind = Observation<ObjectKind>::available(ObjectKind::Directory); after = before;
        first_location.family = LocationQueryFamily::LinuxDescriptor; first_location.directory = anchor;
        first_location.filesystem_name = Observation<NativeFeatureIdentifier>::available(*NativeFeatureIdentifier::from_posix("ext4"));
        first_location.filesystem_type_code = Observation<std::uint64_t>::available(0xef53);
        first_location.linux_directory_inode_flags = Observation<std::uint32_t>::available(0);
        last_location = first_location;
    }
    template<class T> Observation<T> query(int operation, Observation<T> result) {
        const auto i = calls.size(); calls.push_back(operation);
        if (hook) hook(i);
        if (i < failures.size() && failures[i]) return Observation<T>::unavailable(failures[i]->state, failures[i]->error);
        return result;
    }
};
class Cursor final : public DirectoryCursor {
public:
    explicit Cursor(std::shared_ptr<Script> script) : DirectoryCursor(script->anchor), script_(std::move(script)) { ++script_->live; }
    ~Cursor() override { --script_->live; }
    Observation<BasicMetadata> inspect_basic() const override {
        return script_->query(0, Observation<BasicMetadata>::available(script_->basics++ == 0 ? script_->before : script_->after));
    }
    Observation<LocationInfo> inspect_location() const override {
        return script_->query(1, Observation<LocationInfo>::available(script_->locations++ == 0 ? script_->first_location : script_->last_location));
    }
    Observation<DirectoryAccessCheck> check_access(DirectoryAccess access) const override {
        return script_->query(2, Observation<DirectoryAccessCheck>::available({script_->wrong_access ? DirectoryAccess::Search : access,
            script_->wrong_family ? static_cast<DirectoryAccessFamily>(255) : DirectoryAccessFamily::LinuxFaccessat2, script_->id, 1}));
    }
    Observation<DirectoryFscryptPolicy> inspect_fscrypt_policy() const override { return script_->query(3, script_->policy); }
    Observation<DirectoryFscryptKeyStatus> inspect_fscrypt_key_status() const override { return script_->query(4, script_->key); }
protected:
    Observation<DirectoryStep> next_step() override { throw std::runtime_error("inventory attempted enumeration"); }
private:
    std::shared_ptr<Script> script_;
};
auto open(const std::shared_ptr<Script>& script) { return std::make_shared<Cursor>(script); }
DirectoryInventoryJob::Result finish(DirectoryInventoryJob& job) {
    for (int i = 0; i < 100; ++i) {
        if (job.advance() == State::Complete) return take(job.completed());
        if (job.state() == State::Failed) throw std::runtime_error("directory inventory job failed");
    }
    throw std::runtime_error("directory inventory advance limit exceeded");
}
FscryptPolicyData encrypted_policy() {
    std::array<std::byte, 24> bytes{}; bytes[0] = std::byte{2}; return take(FscryptPolicyData::decode(bytes));
}
void set_failure(BasicMetadata& m, int field) {
    const auto set = []<class T>(Observation<T>& value) { value = Observation<T>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "p"}); };
    switch (field) {
    case 0: set(m.identity); break; case 1: set(m.kind); break; case 2: set(m.logical_size); break;
    case 3: set(m.allocated_size); break; case 4: set(m.link_count); break; case 5: set(m.posix_uid); break;
    case 6: set(m.posix_gid); break; case 7: set(m.posix_permissions); break; case 8: set(m.bsd_flags); break;
    case 9: set(m.windows_attributes); break; case 10: set(m.reparse_tag); break; case 11: set(m.linux_attributes); break;
    case 12: set(m.mount_id); break; case 13: set(m.accessed); break; case 14: set(m.modified); break;
    case 15: set(m.changed); break; case 16: set(m.created); break;
    }
}
void set_failure(LocationInfo& m, int field) {
    const auto set = []<class T>(Observation<T>& value) { value = Observation<T>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "p"}); };
    switch (field) {
    case 0: set(m.directory); break; case 1: set(m.filesystem_name); break; case 2: set(m.filesystem_type_code); break;
    case 3: set(m.filesystem_query_id); break; case 4: set(m.volume_query_serial); break; case 5: set(m.features); break;
    case 6: set(m.native_mount_flags); break; case 7: set(m.read_only_mount); break; case 8: set(m.case_sensitive_directory); break;
    case 9: set(m.case_preserving_names); break; case 10: set(m.windows_directory_case_flags); break; case 11: set(m.linux_directory_inode_flags); break;
    case 12: set(m.posix_name_max); break; case 13: set(m.posix_name_chars_max); break; case 14: set(m.posix_path_max); break;
    case 15: set(m.windows_component_utf16_max); break; case 16: set(m.space_accounting_unit); break; case 17: set(m.total_bytes); break;
    case 18: set(m.free_bytes); break; case 19: set(m.available_bytes); break;
    }
}
}
CK_TEST(directory_inventory_collects_fixed_families_and_retains_the_worker_owner) {
    auto script = std::make_shared<Script>(); DirectoryInventoryJob job(open(script), {}, script->clock);
    CK_CHECK(!job.completed().value()); auto result = finish(job); const auto& data = result->data();
    CK_CHECK((script->calls == std::vector<int>{0,1,2,2,2,2,3,4,1,0}));
    CK_CHECK(data.location_before.value() && data.location_after.value() && data.fscrypt_policy.value());
    CK_CHECK(data.fscrypt_key.state() == ObservationState::Unsupported && data.variable_bytes == 17);
    CK_CHECK(take(data.directory_continuity) == IdentityComparison::Same && data.name_rule.value());
    for (std::size_t i = 0; i < data.access.size(); ++i) {
        CK_CHECK(data.access[i].value()->requested == inventory_directory_access[i]);
        CK_CHECK(take(result->compare_owner(Observation<ObjectIdentity>::available(data.access[i].value()->directory))) == IdentityComparison::Same);
    }
    CK_CHECK(script->live == 1 && result->pin_directory());
    const auto calls = script->calls.size(); CK_CHECK(job.advance() == State::Complete && script->calls.size() == calls);
    CK_CHECK(job.abandon() && !job.completed().value() && script->live == 1);
    result.reset(); CK_CHECK(script->live == 0 && !job.abandon());
}
CK_TEST(directory_inventory_preserves_all_native_failures_as_independent_observations) {
    for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        auto script = std::make_shared<Script>(); const NativeError error{ErrorDomain::Posix, 123, "injected.directory"};
        for (auto& failure : script->failures) failure = UnavailableObservation{state, error};
        DirectoryInventoryJob job(open(script), {}, script->clock); const auto result = finish(job); const auto& data = result->data();
        CK_CHECK(script->calls.size() == 10 && data.before.state() == state && data.after.state() == state);
        CK_CHECK(data.location_before.state() == state && data.location_after.state() == state && data.fscrypt_policy.state() == state && data.fscrypt_key.state() == state);
        for (const auto& access : data.access) CK_CHECK(access.state() == state && access.unavailable_detail()->error == error);
        CK_CHECK(data.directory_continuity.state() == state && data.name_rule.state() == state && result->compare_owner(script->anchor).state() == state);
    }
}
CK_TEST(directory_inventory_cancellation_at_every_native_boundary_commits_once_without_partial_results) {
    for (std::size_t boundary = 0; boundary < 10; ++boundary) {
        auto script = std::make_shared<Script>(); std::stop_source stop;
        script->hook = [&](std::size_t i) { if (i == boundary) stop.request_stop(); };
        DirectoryInventoryJob job(open(script), {}, script->clock);
        CK_CHECK(job.advance(stop.get_token()) == State::Paused && !job.completed().value() && script->calls.size() == boundary + 1);
        CK_CHECK(job.advance(stop.get_token()) == State::Paused && script->calls.size() == boundary + 1);
        CK_CHECK(finish(job) && script->calls.size() == 10);
    }
}
CK_TEST(directory_inventory_slow_queries_yield_without_retrying_completed_observations) {
    for (std::size_t boundary = 0; boundary < 10; ++boundary) {
        auto script = std::make_shared<Script>();
        script->hook = [&](std::size_t i) { if (i == boundary) script->clock->advance(std::chrono::milliseconds(20)); };
        DirectoryInventoryJob job(open(script), {}, script->clock);
        CK_CHECK(job.advance() == State::Collecting && !job.completed().value() && script->calls.size() == boundary + 1);
        CK_CHECK(finish(job) && script->calls.size() == 10);
    }
}
CK_TEST(directory_inventory_exceptions_poison_staging_and_do_not_repeat_native_effects) {
    for (std::size_t boundary = 0; boundary < 10; ++boundary) {
        auto script = std::make_shared<Script>(); script->hook = [&](std::size_t i) { if (i == boundary) throw std::bad_alloc{}; };
        DirectoryInventoryJob job(open(script), {}, script->clock); bool caught = false;
        try { (void)job.advance(); } catch (const std::bad_alloc&) { caught = true; }
        CK_CHECK(caught && job.state() == State::Failed && !job.completed().value() && script->live == 0);
        const auto calls = script->calls.size(); CK_CHECK(job.advance() == State::Failed && script->calls.size() == calls && !job.abandon());
    }
}
CK_TEST(directory_inventory_rejects_clock_reversal_and_releases_paused_abandoned_work) {
    for (int mode = 0; mode < 3; ++mode) {
        auto script = std::make_shared<Script>();
        if (mode == 0) script->clock->per_read = -std::chrono::milliseconds(1);
        if (mode == 1) script->clock->throw_on_read = true;
        DirectoryInventoryJob job(open(script), {}, script->clock);
        if (mode == 2) {
            std::stop_source stop; stop.request_stop(); CK_CHECK(job.advance(stop.get_token()) == State::Paused && script->calls.empty());
            CK_CHECK(job.abandon() && script->live == 0 && job.advance() == State::Abandoned);
        } else {
            bool caught = false; try { (void)job.advance(); } catch (const std::runtime_error&) { caught = true; }
            CK_CHECK(caught == (mode == 1) && job.state() == State::Failed && script->live == 0 && !job.completed().value());
        }
    }
}
CK_TEST(directory_inventory_does_not_promote_changed_or_unqualified_contexts_into_owner_matches) {
    for (int mode = 0; mode < 6; ++mode) {
        auto script = std::make_shared<Script>(); auto other = script->id;
        if (mode == 0) other.object = NativeIdentifier::from_u64(99);
        if (mode == 1) other.scope = AdapterScope{};
        if (mode == 2) other.object = {};
        if (mode <= 2) script->after.identity = Observation<ObjectIdentity>::available(other);
        if (mode == 3) script->anchor = Observation<ObjectIdentity>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "anchor"});
        if (mode == 4) script->before.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        if (mode == 5) script->before.identity = Observation<ObjectIdentity>::available({script->id.scope, script->id.scheme, script->id.volume, NativeIdentifier::from_u64(99), {}});
        DirectoryInventoryJob job(open(script), {}, script->clock); const auto result = finish(job);
        CK_CHECK(!result->data().name_rule.value() && !result->compare_owner(Observation<ObjectIdentity>::available(script->id)).value());
        if (mode == 0 || mode == 5) CK_CHECK(take(result->data().directory_continuity) == IdentityComparison::Different);
    }
    auto script = std::make_shared<Script>(); DirectoryInventoryJob job(open(script), {}, script->clock); auto result = finish(job);
    auto other = script->id; other.object = NativeIdentifier::from_u64(99);
    CK_CHECK(take(result->compare_owner(Observation<ObjectIdentity>::available(other))) == IdentityComparison::Different);
    other.scope = AdapterScope{}; CK_CHECK(result->compare_owner(Observation<ObjectIdentity>::available(other)).state() == ObservationState::Unknown);
    CK_CHECK(result->compare_owner(Observation<ObjectIdentity>::unavailable(ObservationState::Denied)).state() == ObservationState::Denied);
}
CK_TEST(directory_inventory_rejects_malformed_reply_enums_and_wrong_access_requests) {
    for (int mode = 0; mode < 6; ++mode) {
        auto script = std::make_shared<Script>();
        if (mode == 0) script->before.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        if (mode == 1) script->after.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        if (mode == 2) script->first_location.family = static_cast<LocationQueryFamily>(255);
        if (mode == 3) { NativeVolumeFeatures features; features.domain = static_cast<VolumeFeatureDomain>(255); script->last_location.features = Observation<NativeVolumeFeatures>::available(features); }
        if (mode == 4) script->wrong_access = true;
        if (mode == 5) script->wrong_family = true;
        DirectoryInventoryJob job(open(script), {}, script->clock); CK_CHECK(job.advance() == State::Failed && !job.completed().value() && script->live == 0);
        CK_CHECK(job.failure()->error.code == static_cast<std::int64_t>(AdapterError::InvalidNativeResult));
    }
}
CK_TEST(directory_inventory_bounds_all_metadata_diagnostics_and_native_identifier_encodings) {
    for (int field = 0; field < 17; ++field) for (const bool after : {false, true}) {
        auto probe = std::make_shared<Script>(); set_failure(after ? probe->after : probe->before, field);
        DirectoryInventoryJob sample(open(probe), {}, probe->clock); const auto bytes = finish(sample)->data().variable_bytes;
        CK_CHECK(bytes == (field < 2 ? 20U : 18U));
        auto script = std::make_shared<Script>(); set_failure(after ? script->after : script->before, field);
        DirectoryInventoryLimits limits; limits.variable_bytes = bytes - 1;
        DirectoryInventoryJob bounded(open(script), limits, script->clock);
        CK_CHECK(bounded.advance() == State::Failed && !bounded.completed().value() && script->live == 0);
        CK_CHECK(bounded.failure()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
    }
    for (int encoding = 0; encoding < 3; ++encoding) {
        auto script = std::make_shared<Script>();
        auto name = encoding == 0 ? *NativeFeatureIdentifier::from_posix("ext4") : encoding == 1 ?
            *NativeFeatureIdentifier::from_windows(u"ext4") : *NativeFeatureIdentifier::from_windows_ea("ext4");
        script->first_location.filesystem_name = Observation<NativeFeatureIdentifier>::available(name); script->last_location = script->first_location;
        DirectoryInventoryLimits limits; limits.variable_bytes = encoding == 1 ? 25 : 17;
        DirectoryInventoryJob job(open(script), limits, script->clock); CK_CHECK(finish(job)->data().variable_bytes == limits.variable_bytes);
    }
}
CK_TEST(directory_inventory_excludes_plain_name_rules_when_encryption_observations_disagree) {
    for (int mode = 0; mode < 4; ++mode) {
        auto script = std::make_shared<Script>(); auto owner = script->id;
        if (mode == 1) owner.object = NativeIdentifier::from_u64(99);
        if (mode == 2) owner.scope = AdapterScope{};
        if (mode == 3) script->key = Observation<DirectoryFscryptKeyStatus>::available({owner, encrypted_policy(), {1, 0, 0, {}}});
        else script->policy = Observation<DirectoryFscryptPolicy>::available({owner, encrypted_policy()});
        DirectoryInventoryJob job(open(script), {}, script->clock); auto result = finish(job);
        CK_CHECK(result->data().name_rule.state() == (mode == 2 ? ObservationState::Unknown : ObservationState::Changed));
        CK_CHECK(take(result->data().directory_continuity) == IdentityComparison::Same);
    }
}
CK_TEST(directory_inventory_validates_limits_directory_and_clock_before_queries) {
    for (int mode = 0; mode < 6; ++mode) {
        auto script = std::make_shared<Script>(); DirectoryInventoryLimits limits;
        if (mode == 0) limits.variable_bytes = 0;
        if (mode == 1) limits.variable_bytes = DirectoryInventoryLimits::hard_variable_bytes + 1;
        if (mode == 2) limits.time_slice = MonotonicClock::Duration::zero();
        if (mode == 3) limits.time_slice = std::chrono::seconds(2);
        bool caught = false;
        try { DirectoryInventoryJob job(mode == 4 ? std::shared_ptr<const DirectoryCursor>{} : open(script), limits,
            mode == 5 ? std::shared_ptr<const MonotonicClock>{} : script->clock); }
        catch (const std::invalid_argument&) { caught = true; }
        CK_CHECK(caught && script->calls.empty() && script->live == 0);
    }
}

CK_TEST(directory_inventory_accounts_every_location_field_and_top_level_query_diagnostic) {
    for (int field = 0; field < 20; ++field) for (bool after : {false, true}) {
        // Baseline is two 4-byte identifiers + 6-byte absence witness + 3-byte
        // key diagnostic. Required name-rule failures additionally copy "p".
        const std::size_t expected = field == 1 ? 14U : (field == 0 || field == 2 || field == 11) ? 19U : 18U;
        auto script = std::make_shared<Script>(); set_failure(after ? script->last_location : script->first_location, field);
        DirectoryInventoryLimits limits; limits.variable_bytes = expected;
        DirectoryInventoryJob job(open(script), limits, script->clock); CK_CHECK(finish(job)->data().variable_bytes == expected);
        auto bounded = std::make_shared<Script>(); set_failure(after ? bounded->last_location : bounded->first_location, field);
        limits.variable_bytes = expected - 1; DirectoryInventoryJob fail(open(bounded), limits, bounded->clock);
        CK_CHECK(fail.advance() == State::Failed && !fail.completed().value() && bounded->live == 0);
    }
    for (std::size_t query = 0; query < 10; ++query) {
        const std::size_t expected = query == 0 || query == 9 ? 20U : query == 1 || query == 8 ? 15U : query == 6 ? 12U : query == 7 ? 15U : 18U;
        auto script = std::make_shared<Script>(); script->failures[query] = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "p"}};
        DirectoryInventoryLimits limits; limits.variable_bytes = expected;
        DirectoryInventoryJob job(open(script), limits, script->clock); CK_CHECK(finish(job)->data().variable_bytes == expected);
    }
    auto script = std::make_shared<Script>(); script->anchor = Observation<ObjectIdentity>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, std::string(100, 'x')});
    DirectoryInventoryLimits limits; limits.variable_bytes = 99; DirectoryInventoryJob job(open(script), limits, script->clock);
    CK_CHECK(job.advance() == State::Failed && script->calls.empty() && script->live == 0);
}
