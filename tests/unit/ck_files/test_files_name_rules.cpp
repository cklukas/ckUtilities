// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_name_rules.hpp"
#include "cvision/testing/cktest.hpp"
#include <functional>
#include <stdexcept>
namespace {
using namespace ck::files;
using Location = Observation<LocationInfo>;
ObjectIdentity identity() { return {AdapterScope{}, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}}; }
LocationInfo location(const ObjectIdentity& id) {
    LocationInfo info; info.family = LocationQueryFamily::LinuxDescriptor;
    info.directory = Observation<ObjectIdentity>::available(id);
    info.filesystem_type_code = Observation<std::uint64_t>::available(0xef53);
    info.linux_directory_inode_flags = Observation<std::uint32_t>::available(0);
    return info;
}
class Cursor final : public DirectoryCursor {
public:
    explicit Cursor(const ObjectIdentity& id) : DirectoryCursor(Observation<ObjectIdentity>::available(id)), replies{Location::available(location(id)), Location::available(location(id))} {}
    mutable std::size_t calls = 0;
    std::array<Location, 2> replies;
    std::function<void(std::size_t)> hook;
    Observation<LocationInfo> inspect_location() const override {
        const auto at = calls++; if (hook) hook(at);
        if (at >= replies.size()) throw std::runtime_error("unexpected repeated location query");
        return replies[at];
    }
private:
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required name-rule fixture observation unavailable");
    return std::move(value).take_value();
}
NativeName name(std::string bytes) { return *NativeName::from_posix(std::move(bytes)); }
auto qualify(const ObjectIdentity& id, const LocationInfo& a, const LocationInfo& b) {
    return detail::qualify_directory_name_rule(Observation<ObjectIdentity>::available(id), Location::available(a), Location::available(b));
}
}
CK_TEST(name_rules_plain_extended_filesystem_requires_qualified_native_context_and_both_reports) {
    const auto id = identity(); auto cursor = std::make_shared<Cursor>(id);
    cursor->replies[1] = Location::available(location(id));
    auto probe = take(DirectoryNameRulesProbe::inspect(cursor));
    CK_CHECK(cursor->calls == 2 && probe->before().value() && probe->after().value());
    const auto& rule = *probe->rule().value();
    CK_CHECK(rule.rule == DirectoryNameRule::LinuxExtendedFsBytewise && rule.filesystem_type == 0xef53);
    CK_CHECK(compare_identity(rule.directory, id) == IdentityComparison::Same);
    CK_CHECK(take(probe->compare(name("A"), name("a"))).relation == ComponentNameRelation::DistinctUnderReportedRules);
    CK_CHECK(take(probe->compare(name("\xc3\xa9"), name("e\xcc\x81"))).relation == ComponentNameRelation::DistinctUnderReportedRules);
    CK_CHECK(take(probe->compare(name("\xff"), name("\xfe"))).relation == ComponentNameRelation::DistinctUnderReportedRules);
    CK_CHECK(take(probe->compare(name("same"), name("same"))).relation == ComponentNameRelation::IdenticalNativeUnits);
    auto a = location(id), b = location(id); b.linux_directory_inode_flags = Observation<std::uint32_t>::available(0x00800000U);
    CK_CHECK(qualify(id, a, b).value()); // NoCow does not change name matching.
}
CK_TEST(name_rules_casefold_encryption_unknown_families_and_foreign_filesystems_never_become_byte_rules) {
    const auto id = identity();
    for (const auto flags : {0x40000000U, 0x800U, 0x40000800U}) {
        auto info = location(id); info.linux_directory_inode_flags = Observation<std::uint32_t>::available(flags);
        CK_CHECK(qualify(id, info, info).state() == ObservationState::Unsupported);
    }
    for (const auto family : {LocationQueryFamily::Unknown, LocationQueryFamily::DarwinDescriptor, LocationQueryFamily::WindowsHandle, static_cast<LocationQueryFamily>(255)}) {
        auto info = location(id); info.family = family; info.case_sensitive_directory = Observation<bool>::available(true);
        const auto result = qualify(id, info, info);
        CK_CHECK(result.state() == (family == LocationQueryFamily::Unknown ? ObservationState::Unknown :
            family == static_cast<LocationQueryFamily>(255) ? ObservationState::Failed : ObservationState::Unsupported));
    }
    auto info = location(id); info.filesystem_type_code = Observation<std::uint64_t>::available(0x9123683e); // btrfs is not qualified here.
    CK_CHECK(qualify(id, info, info).state() == ObservationState::Unsupported);
    info = location(id); info.case_sensitive_directory = Observation<bool>::available(false);
    CK_CHECK(qualify(id, info, info).state() == ObservationState::Changed);
}
CK_TEST(name_rules_changes_in_family_filesystem_relevant_flags_or_directory_identity_are_explicit) {
    const auto id = identity(); const auto before = location(id);
    for (int variant = 0; variant < 10; ++variant) {
        auto after = before; auto changed = id;
        if (variant == 0) after.family = LocationQueryFamily::DarwinDescriptor;
        if (variant == 1) after.filesystem_type_code = Observation<std::uint64_t>::available(1);
        if (variant == 2) after.linux_directory_inode_flags = Observation<std::uint32_t>::available(0x800);
        if (variant == 3) after.linux_directory_inode_flags = Observation<std::uint32_t>::available(0x40000000);
        if (variant == 4) changed.object = NativeIdentifier::from_u64(3);
        if (variant == 5) changed.volume = NativeIdentifier::from_u64(3);
        if (variant == 6) changed.scope = AdapterScope{};
        if (variant == 7) changed.object = {};
        if (variant == 8) changed.generation = 1;
        if (variant == 9) changed.scheme = IdentityScheme::Unknown;
        after.directory = Observation<ObjectIdentity>::available(changed);
        CK_CHECK(qualify(id, before, after).state() == (variant < 6 ? ObservationState::Changed : ObservationState::Unknown));
    }
    auto windows_id = id; windows_id.scheme = IdentityScheme::WindowsFileId128;
    const auto malformed = location(windows_id);
    CK_CHECK(qualify(windows_id, malformed, malformed).state() == ObservationState::Unknown);
}
CK_TEST(name_rules_every_query_and_required_field_failure_keeps_its_native_state_and_error) {
    const auto id = identity(); const auto info = location(id);
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        const NativeError error{ErrorDomain::Posix, 13, "injected.name_rules"};
        for (int field = 0; field < 5; ++field) for (bool second : {false, true}) {
            auto expected = Observation<ObjectIdentity>::available(id);
            auto before = Location::available(info), after = Location::available(info);
            auto changed = info;
            if (field == 0) expected = Observation<ObjectIdentity>::unavailable(state, error);
            if (field == 1) { if (second) after = Location::unavailable(state, error); else before = Location::unavailable(state, error); }
            if (field == 2) changed.directory = Observation<ObjectIdentity>::unavailable(state, error);
            if (field == 3) changed.filesystem_type_code = Observation<std::uint64_t>::unavailable(state, error);
            if (field == 4) changed.linux_directory_inode_flags = Observation<std::uint32_t>::unavailable(state, error);
            if (field >= 2) { if (second) after = Location::available(changed); else before = Location::available(changed); }
            const auto result = detail::qualify_directory_name_rule(expected, before, after);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
        }
    }
}
CK_TEST(name_rules_unavailable_policies_are_not_retried_and_identical_units_stay_only_lexical) {
    auto cursor = std::make_shared<Cursor>(identity()); const NativeError error{ErrorDomain::Posix, 13, "location.denied"};
    cursor->replies[0] = Location::unavailable(ObservationState::Denied, error);
    auto probe = take(DirectoryNameRulesProbe::inspect(cursor));
    CK_CHECK(cursor->calls == 1 && probe->rule().state() == ObservationState::Denied);
    CK_CHECK(probe->after().state() == ObservationState::Unknown && probe->after().unavailable_detail()->error.operation == "name_rules.after_not_attempted");
    const auto identical = take(probe->compare(name("x"), name("x")));
    CK_CHECK(identical.relation == ComponentNameRelation::IdenticalNativeUnits && identical.directory.state() == ObservationState::Denied);
    CK_CHECK(identical.directory.unavailable_detail()->error == error);
    const auto different = probe->compare(name("x"), name("X"));
    CK_CHECK(different.state() == ObservationState::Denied && different.unavailable_detail()->error == error);
    auto unknown = std::make_shared<Cursor>(identity()); auto native = *unknown->replies[0].value(); native.family = LocationQueryFamily::WindowsHandle;
    unknown->replies[0] = Location::available(native);
    const auto windows = take(DirectoryNameRulesProbe::inspect(unknown));
    CK_CHECK(unknown->calls == 1 && windows->rule().state() == ObservationState::Unsupported);
    const auto a = *NativeName::from_windows(u"a");
    CK_CHECK(take(windows->compare(a, a)).relation == ComponentNameRelation::IdenticalNativeUnits);
    CK_CHECK(windows->compare(a, *NativeName::from_windows(u"A")).state() == ObservationState::Unsupported);
}
CK_TEST(name_rules_component_budgets_validation_and_encoding_never_rewrite_names) {
    auto probe = take(DirectoryNameRulesProbe::inspect(std::make_shared<Cursor>(identity())));
    CK_CHECK(probe->compare(name("x"), name("x"), {0}).state() == ObservationState::Failed);
    CK_CHECK(probe->compare(name("x"), name("x"), {NameMappingLimits::hard_native_bytes + 1}).state() == ObservationState::Failed);
    CK_CHECK(probe->compare(name("xx"), name("x"), {1}).state() == ObservationState::Unknown);
    CK_CHECK(probe->compare(name("x"), name("xx"), {1}).state() == ObservationState::Unknown);
    const auto win = *NativeName::from_windows(u"x");
    CK_CHECK(probe->compare(win, win, {1}).state() == ObservationState::Unknown);
    CK_CHECK(take(probe->compare(win, win, {2})).relation == ComponentNameRelation::IdenticalNativeUnits);
    CK_CHECK(probe->compare(win, *NativeName::from_windows(u"X")).state() == ObservationState::Unsupported);
    CK_CHECK(probe->compare(name("x"), win).state() == ObservationState::Failed);
    auto invalid = name("moved"); auto retained = std::move(invalid); (void)retained;
    if (invalid.is_valid()) { // Standard moved-from strings may retain content.
        CK_CHECK(probe->compare(invalid, invalid).value());
    } else CK_CHECK(probe->compare(invalid, invalid).state() == ObservationState::Failed);
}
CK_TEST(name_rules_cancellation_has_no_partial_probe_and_never_repeats_queries_implicitly) {
    for (int cancel_at = -1; cancel_at < 2; ++cancel_at) {
        std::stop_source stop; auto cursor = std::make_shared<Cursor>(identity());
        if (cancel_at == -1) stop.request_stop();
        else cursor->hook = [&](std::size_t at) { if (at == static_cast<std::size_t>(cancel_at)) stop.request_stop(); };
        const auto result = DirectoryNameRulesProbe::inspect(cursor, stop.get_token());
        CK_CHECK(result.state() == ObservationState::Unknown && result.unavailable_detail()->error.operation == "name_rules.cancelled");
        CK_CHECK(cursor->calls == static_cast<std::size_t>(cancel_at + 1));
    }
    CK_CHECK(DirectoryNameRulesProbe::inspect({}).state() == ObservationState::Failed);
}
CK_TEST(name_rules_probe_owns_only_its_held_directory_and_comparisons_do_not_extend_native_lifetimes) {
    auto cursor = std::make_shared<Cursor>(identity()); std::weak_ptr<Cursor> weak = cursor;
    auto probe = take(DirectoryNameRulesProbe::inspect(cursor)); cursor.reset(); CK_CHECK(!weak.expired());
    auto compared = probe->compare(name("a"), name("b")); probe.reset(); CK_CHECK(weak.expired());
    CK_CHECK(compared.value() && compared.value()->relation == ComponentNameRelation::DistinctUnderReportedRules);
}
CK_TEST(name_rules_query_exceptions_release_private_probe_ownership_without_issuing_partial_evidence) {
    for (int query = 0; query < 2; ++query) for (bool allocation : {false, true}) {
        auto cursor = std::make_shared<Cursor>(identity()); std::weak_ptr<Cursor> weak = cursor;
        cursor->hook = [=](std::size_t at) { if (at == static_cast<std::size_t>(query)) { if (allocation) throw std::bad_alloc{}; throw std::runtime_error("injected query exception"); } };
        bool thrown = false; try { (void)DirectoryNameRulesProbe::inspect(cursor); } catch (const std::exception&) { thrown = true; }
        CK_CHECK(thrown && cursor->calls == static_cast<std::size_t>(query + 1));
        cursor.reset(); CK_CHECK(weak.expired());
    }
}
CK_TEST(name_rules_refresh_rechecks_retained_directory_and_never_rewrites_old_observations) {
    auto cursor = std::make_shared<Cursor>(identity());
    auto old = take(DirectoryNameRulesProbe::inspect(cursor));
    auto changed = *cursor->replies[0].value(); changed.linux_directory_inode_flags = Observation<std::uint32_t>::available(0x40000000);
    cursor->calls = 0; cursor->replies[0] = Location::available(changed);
    auto refreshed = take(old->refresh());
    CK_CHECK(cursor->calls == 1 && refreshed->rule().state() == ObservationState::Unsupported);
    CK_CHECK(old->rule().value() && old->after().value());
    CK_CHECK(take(old->compare(name("A"), name("a"))).relation == ComponentNameRelation::DistinctUnderReportedRules);
    CK_CHECK(refreshed->compare(name("A"), name("a")).state() == ObservationState::Unsupported);
}

CK_TEST(name_rules_second_query_failure_retains_both_reports_and_never_uses_first_as_a_complete_rule) {
    auto cursor = std::make_shared<Cursor>(identity());
    const NativeError error{ErrorDomain::Posix, 5, "location.second_query"};
    cursor->replies[1] = Location::unavailable(ObservationState::Failed, error);
    auto probe = take(DirectoryNameRulesProbe::inspect(cursor));
    CK_CHECK(cursor->calls == 2 && probe->before().value() && probe->after().state() == ObservationState::Failed);
    CK_CHECK(probe->rule().state() == ObservationState::Failed && probe->rule().unavailable_detail()->error == error);
    CK_CHECK(probe->compare(name("a"), name("b")).state() == ObservationState::Failed);
    CK_CHECK(take(probe->compare(name("a"), name("a"))).relation == ComponentNameRelation::IdenticalNativeUnits);
    CK_CHECK(cursor->calls == 2);
}
