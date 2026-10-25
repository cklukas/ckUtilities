// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"

#include <stdexcept>

namespace ck::files {

Observation<AdapterScope> DirectoryCursor::source_scope() const {
    return Observation<AdapterScope>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "directory.source_scope"});
}


Observation<DirectoryFscryptKeyStatus> DirectoryCursor::inspect_fscrypt_key_status() const {
    return Observation<DirectoryFscryptKeyStatus>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "directory.fscrypt_key_status"});
}

Observation<DirectoryFscryptPolicy> DirectoryCursor::inspect_fscrypt_policy() const {
    return Observation<DirectoryFscryptPolicy>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "directory.fscrypt_policy"});
}

Observation<DirectoryAccessCheck> DirectoryCursor::check_access(DirectoryAccess access) const {
    return Observation<DirectoryAccessCheck>::unavailable(valid_directory_access(access) ? ObservationState::Unsupported : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(valid_directory_access(access) ? AdapterError::UnsupportedQuery : AdapterError::InvalidAccess), "directory.check_access"});
}

DirectoryRead DirectoryCursor::read(std::size_t limit, std::stop_token stop) {
    if (limit == 0 || limit > max_batch_size) throw std::invalid_argument("invalid directory batch size");
    if (terminal_error_) return {{}, DirectoryReadState::Error, terminal_error_};
    if (ended_) return {{}, DirectoryReadState::End, std::nullopt};
    DirectoryRead result;
    try {
        result.names.reserve(limit);
        for (std::size_t steps = 0; steps < max_batch_size && result.names.size() < limit; ++steps) {
            if (stop.stop_requested()) {
                result.state = DirectoryReadState::Cancelled;
                return result;
            }
            auto next = next_step();
            if (!next.value()) {
                terminal_error_ = *next.unavailable_detail();
                result.state = DirectoryReadState::Error;
                result.error = terminal_error_;
                return result;
            }
            if (std::holds_alternative<DirectoryEnd>(*next.value())) {
                ended_ = true;
                result.state = DirectoryReadState::End;
                return result;
            }
            if (const auto* name = std::get_if<NativeName>(next.value())) result.names.push_back(*name);
        }
        return result;
    } catch (...) {
        // A native cursor may already have advanced before an allocation or
        // adapter exception. Never resume and report a falsely complete listing.
        // This failure record needs no string allocation during exception handling.
        terminal_error_ = UnavailableObservation{ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}};
        throw;
    }
}

Observation<std::unique_ptr<OpenedEntry>> DirectoryCursor::open_child(const NativeName&, EntryAccess access) const {
    if (!valid_entry_access(access)) return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "open_child.access"});
    return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "open_child"});
}

Observation<ChildLookup> DirectoryCursor::lookup_child(const NativeName& name, ChildLookupMode mode) const {
    if(!name.is_valid()||!valid_child_lookup_mode(mode))
        return Observation<ChildLookup>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter,static_cast<std::int64_t>(AdapterError::InvalidIdentifier),"lookup_child.input"});
    return Observation<ChildLookup>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter,static_cast<std::int64_t>(AdapterError::UnsupportedQuery),"lookup_child.native_query"});
}

Observation<std::unique_ptr<DirectoryCursor>> DirectoryCursor::open_directory_child(const NativeName& name) const {
    using Result = Observation<std::unique_ptr<DirectoryCursor>>;
    if (!name.is_valid()) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "open_directory_child.name"});
    return Result::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "open_directory_child.native_query"});
}

Observation<std::unique_ptr<OpenedEntry>> DirectoryCursor::create_child_file(const NativeName& name) const {
    if (!name.is_valid())
        return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "create_child_file.name"});
    return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "create_child_file.native_query"});
}

Observation<bool> DirectoryCursor::retire_child(const NativeName& name,
                                                const ObjectIdentity& expected_identity) const {
    if (!name.is_valid())
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "retire_child.name"});
    if (compare_identity(expected_identity, expected_identity) != IdentityComparison::Same)
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "retire_child.identity"});
    return Observation<bool>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "retire_child.native_query"});
}

Observation<bool> DirectoryCursor::rename_child_noreplace(const NativeName& name,
                                                           const ObjectIdentity& expected_identity,
                                                           const DirectoryCursor& destination,
                                                           const NativeName& destination_name) const {
    if (!name.is_valid() || !destination_name.is_valid())
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "rename_child_noreplace.name"});
    const auto& source_parent = identity();
    const auto& destination_parent = destination.identity();
    if (compare_identity(expected_identity, expected_identity) != IdentityComparison::Same ||
        !source_parent.value() || !destination_parent.value() ||
        compare_identity(*source_parent.value(), *destination_parent.value()) == IdentityComparison::Different)
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "rename_child_noreplace.identity"});
    return Observation<bool>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "rename_child_noreplace.native_query"});
}

Observation<std::unique_ptr<DirectoryCursor>> DirectoryCursor::create_child_directory(const NativeName& name) const {
    if (!name.is_valid())
        return Observation<std::unique_ptr<DirectoryCursor>>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "create_child_directory.name"});
    return Observation<std::unique_ptr<DirectoryCursor>>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "create_child_directory.native_query"});
}

Observation<LocationInfo> DirectoryCursor::inspect_location() const {
    return Observation<LocationInfo>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "location.native_query"});
}
Observation<BasicMetadata> DirectoryCursor::inspect_basic() const {
    return Observation<BasicMetadata>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "directory.metadata_query"});
}
Observation<std::unique_ptr<OpenedEntry>> DirectoryCursor::open_parent_directory() const {
    return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "parent.native_query"});
}
} // namespace ck::files
