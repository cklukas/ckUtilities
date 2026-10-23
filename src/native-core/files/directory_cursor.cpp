// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"

#include <stdexcept>

namespace ck::files {

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

Observation<LocationInfo> DirectoryCursor::inspect_location() const {
    return Observation<LocationInfo>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "location.native_query"});
}
} // namespace ck::files
