// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "held_child.hpp"
#include <limits>

namespace ck::files {
namespace {
Observation<IdentityComparison> continuity_between(const BasicMetadata& a, const BasicMetadata& b) {
    using Result = Observation<IdentityComparison>;
    for (const auto* id : {&a.identity, &b.identity}) if (!id->value()) {
        const auto& e = *id->unavailable_detail(); return Result::unavailable(e.state, e.error);
    }
    const auto compared = compare_identity(*a.identity.value(), *b.identity.value());
    if (compared == IdentityComparison::Unknown) return Result::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "held_child.identity"});
    if (compared == IdentityComparison::Different) return Result::available(compared);
    for (const auto* kind : {&a.kind, &b.kind}) if (!kind->value()) {
        const auto& e = *kind->unavailable_detail(); return Result::unavailable(e.state, e.error);
    }
    if (*a.kind.value() != *b.kind.value()) return Result::unavailable(ObservationState::Changed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "held_child.kind"});
    return Result::available(IdentityComparison::Same);
}
}
HeldChild::HeldChild(std::shared_ptr<const DirectoryCursor> parent, NativeName name,
    std::shared_ptr<const OpenedEntry> child, BasicMetadata metadata,
    std::shared_ptr<const Family> family, std::uint64_t revision, Observation<IdentityComparison> continuity)
    : parent_(std::move(parent)), name_(std::move(name)), child_(std::move(child)),
      metadata_(std::move(metadata)), family_(std::move(family)), revision_(revision), previous_continuity_(std::move(continuity)) {}
Observation<HeldChild::Pointer> HeldChild::observe(std::shared_ptr<const DirectoryCursor> parent, NativeName name, std::stop_token stop) {
    using Result = Observation<Pointer>;
    if (!parent || !name.is_valid()) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "held_child.parent_or_name"});
    const auto cancelled = [] { return Result::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "held_child.cancelled"}); };
    if (stop.stop_requested()) return cancelled();
    auto opened = parent->open_child(name, EntryAccess::MetadataOnly);
    if (stop.stop_requested()) return cancelled();
    if (!opened.value()) { const auto& e = *opened.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    auto owned = std::move(opened).take_value();
    if (!owned) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "held_child.null_child"});
    std::shared_ptr<const OpenedEntry> child(std::move(owned));
    auto metadata = child->inspect_basic();
    if (stop.stop_requested()) return cancelled();
    if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    return Result::available(Pointer(new const HeldChild(std::move(parent), std::move(name), std::move(child),
        std::move(metadata).take_value(), std::make_shared<const Family>(), 1)));
}
Observation<HeldChild::Pointer> HeldChild::refresh(std::stop_token stop) const {
    using Result = Observation<Pointer>;
    auto next = observe(parent_, name_, stop);
    if (!next.value()) return next;
    auto fresh = std::move(next).take_value();
    // Both handles are alive while comparing. Inspect the old held object again
    // rather than trusting a cached ID if an adapter's identity/kind changed.
    const auto old_now = child_->inspect_basic();
    if (stop.stop_requested()) return Result::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "held_child.cancelled"});
    auto continuity = old_now.value() ? continuity_between(metadata_, *old_now.value()) :
        Observation<IdentityComparison>::unavailable(old_now.unavailable_detail()->state, old_now.unavailable_detail()->error);
    if (continuity.value() && *continuity.value() == IdentityComparison::Same)
        continuity = continuity_between(*old_now.value(), fresh->metadata_);
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) continuity = Observation<IdentityComparison>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "held_child.revision_limit"});
    const bool same = continuity.value() && *continuity.value() == IdentityComparison::Same;
    return Result::available(Pointer(new const HeldChild(parent_, name_, fresh->child_, fresh->metadata_,
        same ? family_ : fresh->family_, same ? revision_ + 1 : 1, std::move(continuity))));
}
bool HeldChild::continues(const HeldChild& previous) const noexcept {
    return family_ == previous.family_ && revision_ > previous.revision_ && parent_.get() == previous.parent_.get() && name_ == previous.name_;
}
Observation<BasicMetadata> HeldChild::inspect_basic() const { return child_->inspect_basic(); }
} // namespace ck::files
