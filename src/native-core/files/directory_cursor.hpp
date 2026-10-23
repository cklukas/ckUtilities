// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_name.hpp"
#include "opened_entry.hpp"

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
    virtual Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name) const;
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
