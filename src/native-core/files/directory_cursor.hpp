// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_name.hpp"
#include "opened_entry.hpp"
#include "child_lookup.hpp"
#include "location_info.hpp"
#include "directory_access.hpp"
#include "fscrypt_policy.hpp"
#include "fscrypt_key_status.hpp"

#include <memory>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <vector>
#include <variant>
#include <utility>

namespace ck::files {

enum class DirectoryReadState { More, End, Cancelled, Error };
struct DirectoryRead {
    std::vector<NativeName> names;
    DirectoryReadState state = DirectoryReadState::More;
    std::optional<UnavailableObservation> error; // present exactly for Error
};

struct DirectoryEnd {};
struct DirectorySkip {};
using DirectoryStep = std::variant<NativeName, DirectoryEnd, DirectorySkip>;

enum class DirectoryLinkPolicy { RejectFinalLink, FollowFinalLink };

// Single-worker, owning cursor. Enumeration order is native and unsorted. Names
// are observations, not frozen entry references or authority to mutate. Directory
// changes can affect enumeration; End does not assert a point-in-time snapshot.
class DirectoryCursor {
public:
    virtual ~DirectoryCursor() = default;
    DirectoryCursor(const DirectoryCursor&) = delete;
    DirectoryCursor& operator=(const DirectoryCursor&) = delete;
    static constexpr std::size_t max_batch_size = 4096;

    // 1..max_batch_size, otherwise invalid_argument before consuming entries.
    // Cancellation is cooperative between native calls, can return a partial
    // batch, and permits resumption. End/errors are sticky. OS calls may block;
    // this must never run in a UI callback. Native cursor storage is also bounded.
    // At most max_batch_size native steps per read, including omitted entries;
    // a More batch may therefore contain fewer names than the requested limit.
    DirectoryRead read(std::size_t limit, std::stop_token stop = {});
    // One native component only, opened without following the final link or
    // reparse point. This captures the object present at open time, not an
    // earlier enumeration snapshot. Revalidate identity before later actions.
    virtual Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name, EntryAccess access = EntryAccess::MetadataOnly) const;
    // Explicit native lookup policy, metadata-only access, no final-link
    // following. Only definitive name-not-found native codes establish Missing;
    // generic Gone/error states do not. No byte/case-fold enumeration fallback.
    virtual Observation<ChildLookup> lookup_child(const NativeName& name, ChildLookupMode mode) const;
    // Open one child as an independently owning enumeration cursor. Always
    // rejects a final link/reparse point, never joins an absolute path or changes
    // cwd. Name resolution is native-default, not exact-spelling certification.
    // No recursive traversal, mount confinement, provider or snapshot guarantee.
    virtual Observation<std::unique_ptr<DirectoryCursor>> open_directory_child(const NativeName& name) const;
    // Live, read-only queries against this held directory. Per-field failures
    // remain explicit; successful advertisements do not authorize operations.
    virtual Observation<LocationInfo> inspect_location() const;
    // Worker-only native check through this held directory, without mutation,
    // original pathname reopening, permission-bit emulation or elevation.
    // Available is a momentary native check, never an operation capability.
    virtual Observation<DirectoryAccessCheck> check_access(DirectoryAccess access) const;
    // Read-only Linux fscrypt directory policy, not whole-volume encryption,
    // key availability or authority to create/replay encrypted data. Worker-only.
    virtual Observation<DirectoryFscryptPolicy> inspect_fscrypt_policy() const;
    // Linux filesystem keyring status for this held directory's exact policy.
    // No keys/passphrases read, added or removed. Policy/identity are bracketed;
    // no process-keyring, content-access, snapshot or encryption replay promise.
    virtual Observation<DirectoryFscryptKeyStatus> inspect_fscrypt_key_status() const;
    // Live metadata/parent queries through the held directory, independent of
    // enumeration position. Parent references have metadata-only access and
    // do not certify a frozen ancestry or namespace confinement.
    virtual Observation<BasicMetadata> inspect_basic() const;
    virtual Observation<std::unique_ptr<OpenedEntry>> open_parent_directory() const;
    // Live adapter namespace, independent of native ID query success. A default
    // injected cursor cannot manufacture a namespace from a child observation.
    // Read-only provenance token, never native acquisition/mutation authority.
    virtual Observation<AdapterScope> source_scope() const;
    const Observation<ObjectIdentity>& identity() const noexcept { return identity_; }

protected:
    explicit DirectoryCursor(Observation<ObjectIdentity> identity = {}) : identity_(std::move(identity)) {}
    // Exactly one native record/query step. Navigation records yield Skip so
    // cancellation is checked even while traversing omitted entries.
    virtual Observation<DirectoryStep> next_step() = 0;

private:
    Observation<ObjectIdentity> identity_;
    bool ended_ = false;
    std::optional<UnavailableObservation> terminal_error_;
};

} // namespace ck::files
