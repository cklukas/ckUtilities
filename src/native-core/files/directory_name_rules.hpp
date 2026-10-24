// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_cursor.hpp"
#include "name_mapping.hpp"
namespace ck::files {
enum class DirectoryNameRule { LinuxExtendedFsBytewise };
struct QualifiedDirectoryNameRule {
    DirectoryNameRule rule;
    ObjectIdentity directory;
    std::uint64_t filesystem_type;
    std::uint32_t before_inode_flags, after_inode_flags;
};
enum class ComponentNameRelation { IdenticalNativeUnits, DistinctUnderReportedRules };
struct ComponentNameComparison {
    ComponentNameRelation relation;
    Observation<ObjectIdentity> directory;
};
namespace detail {
Observation<QualifiedDirectoryNameRule> qualify_directory_name_rule(const Observation<ObjectIdentity>& expected,
    const Observation<LocationInfo>& before, const Observation<LocationInfo>& after);
}
// Worker-owned immutable observations through the same held directory. This is
// a rule observation, not a namespace snapshot, create capability or reservation.
// Distinct names can still be hard links to the same object. Unknown policies
// never fall back to locale/Unicode/OS case conversion or filesystem labels.
class DirectoryNameRulesProbe final {
public:
    using Pointer = std::unique_ptr<const DirectoryNameRulesProbe>;
    static Observation<Pointer> inspect(std::shared_ptr<const DirectoryCursor> directory, std::stop_token stop = {});
    // Independent new observations through the retained directory, never its
    // original pathname. The old record remains unchanged and independently owned.
    Observation<Pointer> refresh(std::stop_token stop = {}) const { return inspect(directory_, stop); }
    DirectoryNameRulesProbe(const DirectoryNameRulesProbe&) = delete;
    DirectoryNameRulesProbe& operator=(const DirectoryNameRulesProbe&) = delete;
    const Observation<LocationInfo>& before() const noexcept { return before_; }
    const Observation<LocationInfo>& after() const noexcept { return after_; }
    const Observation<QualifiedDirectoryNameRule>& rule() const noexcept { return rule_; }
    // Exact units are a lexical witness even with unavailable native rules;
    // that result retains the unavailable directory context. A distinct result
    // requires a qualified rule. Neither establishes name acceptance, object
    // inequality, absence, security or execution-time collision freedom.
    // Same encoding only; no mapping, normalization, truncation or content I/O.
    Observation<ComponentNameComparison> compare(const NativeName& left, const NativeName& right, NameMappingLimits limits = {}) const;
private:
    explicit DirectoryNameRulesProbe(std::shared_ptr<const DirectoryCursor> directory) : directory_(std::move(directory)) {}
    std::shared_ptr<const DirectoryCursor> directory_;
    Observation<LocationInfo> before_, after_;
    Observation<QualifiedDirectoryNameRule> rule_;
};
} // namespace ck::files
