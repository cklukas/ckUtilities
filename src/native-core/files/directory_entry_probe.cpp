// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_entry_probe.hpp"

namespace ck::files {
namespace {
template<class T> Observation<T> invalid(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
template<class T, class U> Observation<T> unavailable_from(const Observation<U>& source) {
    const auto& detail = *source.unavailable_detail();
    return Observation<T>::unavailable(detail.state, detail.error);
}
// Message-context consistency only, including unqualified IDs. This exact
// observation comparison must not be used as semantic native identity proof.
bool same_context(const Observation<ObjectIdentity>& a, const Observation<ObjectIdentity>& b) {
    if (a.state() != b.state()) return false;
    if (!a.value()) return a.unavailable_detail()->error == b.unavailable_detail()->error;
    const auto& x = *a.value(); const auto& y = *b.value();
    return x.scope == y.scope && x.scheme == y.scheme && x.volume == y.volume && x.object == y.object && x.generation == y.generation;
}
}
DirectoryEntryProbe::DirectoryEntryProbe(std::shared_ptr<const DirectoryCursor> directory, NativeName name,
    ChildLookupMode mode, DirectoryEntryProbeLimits limits)
    : directory_(std::move(directory)), requested_(std::move(name)), mode_(mode), limits_(limits) {}
Observation<DirectoryEntryProbe::Pointer> DirectoryEntryProbe::inspect(std::shared_ptr<const DirectoryCursor> directory,
    const NativeName& name, ChildLookupMode mode, DirectoryEntryProbeLimits limits, std::stop_token stop) {
    using Result = Observation<Pointer>;
    if (!directory || !valid_child_lookup_mode(mode))
        return invalid<Pointer>(AdapterError::InvalidIdentifier, "entry_probe.input");
    if (limits.component_bytes == 0 || limits.component_bytes > DirectoryEntryProbeLimits::hard_component_bytes)
        return invalid<Pointer>(AdapterError::InvalidLimit, "entry_probe.limits");
    const auto unit_bytes = name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    const auto units = name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size();
    if (units > limits.component_bytes / unit_bytes)
        return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "entry_probe.component_bytes"});
    if (!name.is_valid()) return invalid<Pointer>(AdapterError::InvalidIdentifier, "entry_probe.name");
    const auto cancelled = [] {
        return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "entry_probe.cancelled"});
    };
    if (stop.stop_requested()) return cancelled();
    auto record = std::unique_ptr<DirectoryEntryProbe>(new DirectoryEntryProbe(std::move(directory), name, mode, limits));
    if (record->requested_.windows_units()) record->win32_ = check_win32_component(*record->requested_.windows_units());
    record->location_ = record->directory_->inspect_location();
    if (stop.stop_requested()) return cancelled();
    record->length_ = record->location_.value() ? assess_component_length(record->requested_, *record->location_.value()) :
        unavailable_from<ComponentLengthAssessment>(record->location_);
    record->lookup_ = record->directory_->lookup_child(record->requested_, mode);
    if (stop.stop_requested()) return cancelled();
    if (record->lookup_.value()) {
        const auto& lookup = *record->lookup_.value();
        const auto* context = lookup.context();
        if (!context || context->requested != record->requested_ || context->mode != mode ||
            !same_context(context->directory, record->directory_->identity()) ||
            (lookup.state() != ChildLookupState::Found && lookup.state() != ChildLookupState::Missing))
            record->lookup_ = invalid<ChildLookup>(AdapterError::InvalidNativeResult, "entry_probe.lookup_context");
    }
    if (!record->lookup_.value()) record->metadata_ = unavailable_from<BasicMetadata>(record->lookup_);
    else if (record->lookup_.value()->state() == ChildLookupState::Found)
        record->metadata_ = record->lookup_.value()->entry()->inspect_basic();
    else record->metadata_ = Observation<BasicMetadata>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "entry_probe.no_existing_entry"});
    if (stop.stop_requested()) return cancelled();
    return Result::available(Pointer(record.release()));
}
Observation<DirectoryEntryProbe::Pointer> DirectoryEntryProbe::refresh(std::stop_token stop) const {
    return inspect(directory_, requested_, mode_, limits_, stop);
}
} // namespace ck::files
