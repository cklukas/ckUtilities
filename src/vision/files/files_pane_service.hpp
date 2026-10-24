// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "indexed_pane.hpp"
#include "native_filesystem.hpp"
#include <functional>
#include <span>
#include <vector>
namespace ck::vision {
// Owner-free requested route: an absolute root address followed by exact native
// components. The root may have normal host path semantics; each descendant is
// resolved through a held cursor. This is neither an object identity nor a flat
// pathname suitable for reopening. In particular, Windows literal components
// must never be appended to an ordinary Win32 path and reopened.
class FilesPaneLocation final {
  public:
    explicit FilesPaneLocation(ck::files::NativeAbsolutePath root);
    const ck::files::NativeAbsolutePath &root_address() const noexcept { return root_; }
    std::span<const ck::files::NativeName> components() const noexcept { return components_; }
    std::size_t native_bytes() const noexcept { return bytes_; }
    std::string display_text() const; // escaped presentation only, never parse back
    // Requested-address navigation, not retained ancestor identity. Exact
    // components remain separate and are replayed through held cursors.
    ck::files::Observation<FilesPaneLocation> parent() const;
    ck::files::Observation<FilesPaneLocation> namespace_root() const;
    ck::files::Observation<FilesPaneLocation>
    descend(const ck::files::NativeName &name, ck::files::NativePathLimits limits, std::size_t max_components) const;
    static constexpr std::size_t hard_components = 4096;
    friend bool operator==(const FilesPaneLocation &, const FilesPaneLocation &) = default;

  private:
    ck::files::NativeAbsolutePath root_;
    std::vector<ck::files::NativeName> components_;
    std::size_t bytes_;
};
class FilesPaneService;
// Process-local request identity, not native identity or operation authority.
class FilesPaneTicket final {
  public:
    FilesPaneTicket() = default;
    std::uint64_t number() const noexcept { return identity_ ? identity_->number : 0; }
    bool valid() const noexcept { return identity_ != nullptr; }
    friend bool operator==(const FilesPaneTicket &, const FilesPaneTicket &) = default;

  private:
    struct Identity {
        std::uint64_t number;
    };
    explicit FilesPaneTicket(std::uint64_t number) : identity_(std::make_shared<const Identity>(Identity{number})) {}
    std::shared_ptr<const Identity> identity_;
    friend class FilesPaneService;
};
struct FilesPaneReadRange {
    std::uint64_t first = 0;
    std::size_t rows = 256;
};
enum class FilesPaneSubmission { Accepted, Busy, Stopped, Invalid, LimitExceeded, Exhausted };
struct FilesPaneSubmitted {
    FilesPaneSubmission state;
    FilesPaneTicket ticket;
};
enum class FilesPaneReplyState { Ready, Failed, Cancelled, Stale, OutOfRange, LimitExceeded, WorkerFault };
enum class FilesPaneWorkerFault { None, Allocation, Exception };
struct FilesPaneFocusRow {
    ck::files::PaneRowId row;
};
struct FilesPaneChangeMarks {
    ck::files::IndexedMarkCommand command;
};
struct FilesPaneChangeView {
    ck::files::PaneView view;
};
using FilesPaneChange = std::variant<FilesPaneFocusRow, FilesPaneChangeMarks, FilesPaneChangeView>;
enum class FilesPaneReplyKind { Navigation, Page, Focus, Marks, View, Fault };
enum class FilesPaneChangeOutcome { NotRequested, NotApplied, Applied, Unknown };
struct FilesPaneReply {
    FilesPaneTicket request, navigation;
    FilesPaneReplyState state = FilesPaneReplyState::Failed;
    ck::files::PaneStateStamp stamp;
    std::shared_ptr<const ck::files::IndexedPanePage> page;
    // Exact requested routes, never held objects or mutable-entry targets.
    // Failed/cancelled navigation retains the active route. A child request's
    // requested route stays absent until its scoped row is actually resolved.
    std::shared_ptr<const FilesPaneLocation> active_location, requested_location;
    std::optional<ck::files::UnavailableObservation> error;
    FilesPaneWorkerFault worker_fault = FilesPaneWorkerFault::None;
    FilesPaneReplyKind kind = FilesPaneReplyKind::Page;
    // A local selection/view change may commit before page capture fails or
    // cancellation arrives. Never equate a missing page with rollback. Failed
    // changes can also carry a current recovery page of the retained state.
    // Terminal worker faults may interrupt acknowledgment after commitment.
    FilesPaneChangeOutcome change_outcome = FilesPaneChangeOutcome::NotRequested;
};
struct FilesPaneServiceLimits {
    std::size_t path_bytes = 64 * 1024;
    std::size_t page_rows = 4096, page_text_bytes = 1024 * 1024;
    std::size_t page_wire_bytes = 4 * 1024 * 1024;
    std::size_t diagnostic_bytes = 64 * 1024;
    std::uint64_t directory_rows = 1'000'000;
    std::uint64_t cache_bytes = 1024ULL * 1024 * 1024;
    std::size_t cache_files = 128;
    std::size_t location_components = 1024;
};
struct FilesPaneBackend {
    // Called lazily on the pane's worker. Capture value configuration only;
    // do not pre-create native owners on the UI thread or retain returned owners.
    std::function<std::unique_ptr<ck::files::BasicFileSystem>()> filesystem;
    std::function<ck::files::Observation<std::shared_ptr<ck::files::IndexFileFactory>>()> workspace;
    // Optional native-home discovery, worker-only. An absent resolver refuses
    // explicitly; it must not cause a guessed location or UI-thread lookup.
    std::function<ck::files::Observation<ck::files::NativeAbsolutePath>(ck::files::NativePathLimits, std::stop_token)>
        home;
    ck::files::StorageModel storage_model = ck::files::StorageModel::Unknown;
    std::shared_ptr<const ck::files::MonotonicClock> clock = ck::files::system_monotonic_clock();
};
// Builds value-only configuration; native source/workspace opening is deferred
// to the worker. The cache root must be explicit and already private/qualified.
FilesPaneBackend native_files_pane_backend(ck::files::NativeAbsolutePath cache_root);
// One serialized worker, latest navigation/page slots and one unread result.
// Submission/take never do native I/O or join. Native calls can block; stop is
// cooperative. Navigation supersedes old work; page requests cannot replace an
// unread result or interrupt a navigation. Delivery is gated by exact tickets.
// Results carry owner-free pages/addresses/stamps only. The UI must bound its
// retained replies and validate both the ticket and page stamp before use.
// wake carries no result and is coalesced until take(); a terminal fault makes
// one final notification attempt. It may run on a worker
// and must marshal UI work through a lifetime-checked callback. It must outlive
// join(). Callback exceptions poison the worker; fault()/running() remain
// queryable even if notification itself fails. Destruction/join request stop and WAIT: the composition root must do
// this outside UI dispatch, after the view/controller has withdrawn callbacks.
class FilesPaneService final {
  public:
    FilesPaneService(FilesPaneBackend backend, FilesPaneServiceLimits limits = {}, std::function<void()> wake = {});
    ~FilesPaneService();
    FilesPaneService(const FilesPaneService &) = delete;
    FilesPaneService &operator=(const FilesPaneService &) = delete;
    FilesPaneSubmitted navigate(ck::files::NativeAbsolutePath location, ck::files::PaneView view = {},
                                std::size_t first_page_rows = 256);
    // Reopen the requested root, then each exact component relative to its
    // held predecessor, rejecting links/reparse points. No pathname flattening
    // or claim of continuity with an earlier visit. Latest navigation wins.
    FilesPaneSubmitted navigate(FilesPaneLocation location, ck::files::PaneView view = {},
                                std::size_t first_page_rows = 256);
    // Latest-navigation semantics, with bounded lazy discovery on the worker.
    FilesPaneSubmitted navigate_home(ck::files::PaneView view = {}, std::size_t first_page_rows = 256);
    // Exclusive navigation from one visible scoped row of the current pane.
    // No I/O at submission, no mark/cursor retargeting or absolute path reopen.
    // Busy while work or an unread reply exists. Absolute navigation may
    // supersede it. Failure/cancellation preserves the previous pane and route.
    FilesPaneSubmitted enter_directory(FilesPaneTicket navigation, ck::files::PaneStateStamp stamp,
                                       ck::files::PaneRowId row, ck::files::PaneView view = {},
                                       std::size_t first_page_rows = 256);
    FilesPaneSubmitted read_page(FilesPaneTicket navigation, ck::files::PaneStateStamp stamp, FilesPaneReadRange range);
    // One acknowledged local-state command; Busy while any work/result is
    // outstanding. It is never coalesced with another change or page request.
    // Navigation may supersede it; cancellation cannot undo a committed change.
    // All row lookup, marks, sorting/filtering and retirement run on the worker.
    FilesPaneSubmitted change(FilesPaneTicket navigation, ck::files::PaneStateStamp stamp, FilesPaneChange command,
                              FilesPaneReadRange range);
    bool cancel(const FilesPaneTicket &request);
    std::optional<FilesPaneReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;
    FilesPaneWorkerFault fault() const noexcept;

  private:
    static FilesPaneTicket ticket(std::uint64_t number) { return FilesPaneTicket(number); }
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ck::vision
