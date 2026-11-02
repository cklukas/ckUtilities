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
    // Worker-only creation of a new regular staging file below this held
    // directory. Existing names, final links/reparse points and non-regular
    // results are refused. The returned handle is not published under a
    // destination name and must be retained by the worker.
    virtual Observation<std::unique_ptr<OpenedEntry>> create_child_file(const NativeName& name) const;
    // Worker-only creation of a new hard-link name for one verified regular
    // file below a held destination directory. The source identity is checked
    // at use, the destination is no-replace, and the adapter must flush the
    // published directory entry before reporting success. Directory hard
    // links, pathname fallbacks and cross-volume emulation are forbidden.
    virtual Observation<bool> create_hard_link(const NativeName& source_name,
                                                const ObjectIdentity& expected_source,
                                                const DirectoryCursor& destination,
                                                const NativeName& destination_name) const;
    // Worker-only creation of one symbolic-link name below this held
    // directory. The target is opaque link payload, never traversed or
    // converted from display text. The destination is no-replace and the
    // adapter must flush and verify the published link before success.
    virtual Observation<bool> create_symbolic_link(const SymbolicLinkTarget& target,
                                                    const NativeName& destination_name) const;
    // Worker-only source retirement boundary. The adapter must open the exact
    // child relative to this held directory, verify expected_identity, and
    // use a handle/object-based disposition. A pathname unlink or a
    // stat-then-unlink emulation is not an implementation of this contract.
    // Unsupported is required until the platform has a qualified protocol.
    virtual Observation<bool> retire_child(const NativeName& name,
                                            const ObjectIdentity& expected_identity) const;
    // Worker-only cleanup of a file created by this operation under the held
    // directory. The adapter must bind the disposition to the verified object
    // handle; a pathname unlink/stat sequence is not an implementation.
    virtual Observation<bool> discard_child(const NativeName& name,
                                             const ObjectIdentity& expected_identity) const;
    // Worker-only native Trash/Recycle Bin disposition. The adapter must
    // verify expected_identity, preserve native restore metadata, and publish
    // through the platform facility without following a final link/reparse
    // point. Unsupported is required when that facility cannot be qualified;
    // callers must not silently fall back to permanent deletion.
    virtual Observation<bool> trash_child(const NativeName& name,
                                           const ObjectIdentity& expected_identity) const;
    // Worker-only same-filesystem no-replace rename below held parents. The
    // adapter must verify the source identity at use, reject an occupied
    // destination, publish through a native no-clobber primitive, verify the
    // destination identity and flush the affected parent directories. A
    // pathname stat-then-rename emulation is not an implementation of this
    // contract; Unsupported is required until the platform is qualified.
    virtual Observation<bool> rename_child_noreplace(const NativeName& name,
                                                      const ObjectIdentity& expected_identity,
                                                      const DirectoryCursor& destination,
                                                      const NativeName& destination_name) const;
    // Worker-only case-only rename within this held directory. The adapter
    // must prove that the destination's native lookup resolves to the same
    // source object, reject hard-link ambiguity, use a native rename and
    // flush the parent directory. A lexical case-fold or path-string test is
    // not sufficient; Unsupported is required when native proof is absent.
    virtual Observation<bool> rename_child_case_only(const NativeName& name,
                                                     const ObjectIdentity& expected_identity,
                                                     const NativeName& destination_name) const;
    // Worker-only creation of a new regular directory below this held
    // directory. Creation is exclusive, final links/reparse points are
    // refused, and the returned cursor owns the created directory handle.
    virtual Observation<std::unique_ptr<DirectoryCursor>> create_child_directory(const NativeName& name) const;
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
