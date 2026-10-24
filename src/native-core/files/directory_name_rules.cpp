// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_name_rules.hpp"
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
template<class T> Observation<T> unavailable(const UnavailableObservation& failure) {
    return Observation<T>::unavailable(failure.state, failure.error);
}
}
Observation<QualifiedDirectoryNameRule> detail::qualify_directory_name_rule(const Observation<ObjectIdentity>& expected,
    const Observation<LocationInfo>& before, const Observation<LocationInfo>& after) {
    using Rule = QualifiedDirectoryNameRule;
    if (!expected.value()) return unavailable<Rule>(*expected.unavailable_detail());
    for (const auto* location : {&before, &after}) {
        if (!location->value()) return unavailable<Rule>(*location->unavailable_detail());
        const auto& info = *location->value();
        if (!info.directory.value()) return unavailable<Rule>(*info.directory.unavailable_detail());
        const auto compared = compare_identity(*expected.value(), *info.directory.value());
        if (compared != IdentityComparison::Same) return error<Rule>(
            compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
            compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity, "name_rules.directory");
        switch (info.family) {
        case LocationQueryFamily::Unknown:
            return error<Rule>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "name_rules.family");
        case LocationQueryFamily::LinuxDescriptor: case LocationQueryFamily::DarwinDescriptor: case LocationQueryFamily::WindowsHandle: break;
        default: return error<Rule>(ObservationState::Failed, AdapterError::InvalidNativeResult, "name_rules.family");
        }
    }
    const auto& a = *before.value(); const auto& b = *after.value();
    if (a.family != b.family) return error<Rule>(ObservationState::Changed, AdapterError::MetadataChanged, "name_rules.family_changed");
    if (a.family != LocationQueryFamily::LinuxDescriptor)
        return error<Rule>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "name_rules.unqualified_family");
    if (expected.value()->scheme != IdentityScheme::PosixDeviceInode)
        return error<Rule>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "name_rules.identity_scheme");
    for (const auto* info : {&a, &b}) {
        if (!info->filesystem_type_code.value()) return unavailable<Rule>(*info->filesystem_type_code.unavailable_detail());
        if (!info->linux_directory_inode_flags.value()) return unavailable<Rule>(*info->linux_directory_inode_flags.unavailable_detail());
    }
    if (*a.filesystem_type_code.value() != *b.filesystem_type_code.value())
        return error<Rule>(ObservationState::Changed, AdapterError::MetadataChanged, "name_rules.filesystem_changed");
    // EXT{2,3,4}_SUPER_MAGIC is shared. These rules are qualified only for the
    // native extended-filesystem family, not every Linux/POSIX filesystem.
    if (*a.filesystem_type_code.value() != 0xef53U)
        return error<Rule>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "name_rules.unqualified_filesystem");
    constexpr std::uint32_t casefold = 0x40000000U, encrypted = 0x800U, relevant = casefold | encrypted;
    const auto flags_a = *a.linux_directory_inode_flags.value(), flags_b = *b.linux_directory_inode_flags.value();
    if ((flags_a & relevant) != (flags_b & relevant))
        return error<Rule>(ObservationState::Changed, AdapterError::MetadataChanged, "name_rules.flags_changed");
    if ((flags_b & relevant) != 0)
        return error<Rule>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "name_rules.casefold_or_encryption");
    for (const auto* info : {&a, &b})
        if (info->case_sensitive_directory.value() && !*info->case_sensitive_directory.value())
            return error<Rule>(ObservationState::Changed, AdapterError::MetadataChanged, "name_rules.conflicting_case_report");
    return Observation<Rule>::available({DirectoryNameRule::LinuxExtendedFsBytewise, *expected.value(), *b.filesystem_type_code.value(), flags_a, flags_b});
}
Observation<DirectoryNameRulesProbe::Pointer> DirectoryNameRulesProbe::inspect(std::shared_ptr<const DirectoryCursor> directory, std::stop_token stop) {
    using Result = Observation<Pointer>;
    if (!directory) return error<Pointer>(ObservationState::Failed, AdapterError::InvalidIdentifier, "name_rules.null_directory");
    const auto cancelled = [] { return error<Pointer>(ObservationState::Unknown, AdapterError::InterruptedRead, "name_rules.cancelled"); };
    if (stop.stop_requested()) return cancelled();
    std::unique_ptr<DirectoryNameRulesProbe> result(new DirectoryNameRulesProbe(std::move(directory)));
    result->before_ = result->directory_->inspect_location();
    if (stop.stop_requested()) return cancelled();
    result->rule_ = detail::qualify_directory_name_rule(result->directory_->identity(), result->before_, result->before_);
    if (!result->rule_.value()) {
        // Do not retry an unavailable/unqualified query. Only a potentially
        // usable rule needs a second independent observation for qualification.
        result->after_ = error<LocationInfo>(ObservationState::Unknown, AdapterError::NotApplicable, "name_rules.after_not_attempted");
        return Result::available(std::move(result));
    }
    if (stop.stop_requested()) return cancelled();
    result->after_ = result->directory_->inspect_location();
    if (stop.stop_requested()) return cancelled();
    result->rule_ = detail::qualify_directory_name_rule(result->directory_->identity(), result->before_, result->after_);
    return Result::available(std::move(result));
}
Observation<ComponentNameComparison> DirectoryNameRulesProbe::compare(const NativeName& left, const NativeName& right, NameMappingLimits limits) const {
    using Result = Observation<ComponentNameComparison>;
    if (limits.max_native_bytes == 0 || limits.max_native_bytes > NameMappingLimits::hard_native_bytes)
        return error<ComponentNameComparison>(ObservationState::Failed, AdapterError::InvalidLimit, "name_rules.component_limit");
    for (const auto* name : {&left, &right}) {
        const auto units = name->posix_bytes() ? name->posix_bytes()->size() : name->windows_units()->size();
        const auto bytes_per_unit = name->posix_bytes() ? std::size_t{1} : sizeof(char16_t);
        if (units > limits.max_native_bytes / bytes_per_unit)
            return error<ComponentNameComparison>(ObservationState::Unknown, AdapterError::InventoryLimit, "name_rules.component_bytes");
        if (!name->is_valid()) return error<ComponentNameComparison>(ObservationState::Failed, AdapterError::InvalidIdentifier, "name_rules.component");
    }
    if (left.encoding() != right.encoding())
        return error<ComponentNameComparison>(ObservationState::Failed, AdapterError::InvalidIdentifier, "name_rules.encoding");
    // This statement is lexical only; it never consumes an unavailable rule as
    // positive native policy evidence or a usable parent identity.
    if (left == right) {
        Observation<ObjectIdentity> context;
        if (before_.value()) context = before_.value()->directory;
        else { const auto& e = *before_.unavailable_detail(); context = Observation<ObjectIdentity>::unavailable(e.state, e.error); }
        return Result::available({ComponentNameRelation::IdenticalNativeUnits, std::move(context)});
    }
    if (!rule_.value()) return unavailable<ComponentNameComparison>(*rule_.unavailable_detail());
    if (rule_.value()->rule != DirectoryNameRule::LinuxExtendedFsBytewise)
        return error<ComponentNameComparison>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "name_rules.rule");
    if (!left.posix_bytes()) return error<ComponentNameComparison>(ObservationState::Unsupported, AdapterError::NotApplicable, "name_rules.rule_encoding");
    return Result::available({ComponentNameRelation::DistinctUnderReportedRules, Observation<ObjectIdentity>::available(rule_.value()->directory)});
}
} // namespace ck::files
