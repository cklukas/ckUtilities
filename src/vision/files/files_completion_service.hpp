// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_completion_index.hpp"
#include "files_pane_service.hpp"
namespace ck::vision {
class FilesCompletionService;
class FilesCompletionTicket final {
  public:
    FilesCompletionTicket() = default;
    bool valid() const noexcept { return identity_ != nullptr; }
    std::uint64_t number() const noexcept { return identity_ ? *identity_ : 0; }
    friend bool operator==(const FilesCompletionTicket &, const FilesCompletionTicket &) = default;

  private:
    friend class FilesCompletionService;
    explicit FilesCompletionTicket(std::uint64_t number) : identity_(std::make_shared<const std::uint64_t>(number)) {}
    std::shared_ptr<const std::uint64_t> identity_;
};
enum class FilesCompletionSubmission { Accepted, Invalid, LimitExceeded, Stopped, Exhausted };
enum class FilesCompletionReplyState { Ready, Failed, Cancelled, Stale, LimitExceeded, WorkerFault };
struct FilesCompletionSubmitted {
    FilesCompletionSubmission state;
    FilesCompletionTicket ticket;
};
struct FilesCompletionReply {
    FilesCompletionTicket request;
    std::uint64_t revision = 0;
    FilesCompletionReplyState state = FilesCompletionReplyState::Failed;
    std::shared_ptr<const FilesCompletionPage> page;
    std::optional<ck::files::UnavailableObservation> error;
    FilesPaneWorkerFault fault = FilesPaneWorkerFault::None;
};
struct FilesCompletionServiceLimits {
    FilesLocationTextLimits query{{64 * 1024}, 1024, 32768};
    FilesCompletionIndexLimits matches;
    std::uint64_t directory_rows = 1'000'000, cache_bytes = 1024ULL * 1024 * 1024;
    std::size_t cache_files = 128, diagnostic_bytes = 64 * 1024;
};
// One serialized worker, one latest pending request and one unread value reply.
// Uses the same lazy filesystem/private-workspace boundary as pane services.
// Native owners, discovery, matching and cache retirement stay on that worker.
// Same-parent prefix edits reuse in-flight discovery; matching replies are never
// relabeled. All deliveries require the request ticket AND editor revision.
// An explicit refresh discards the targeted generation. No live subscription,
// snapshot, object identity or operation permission is inferred from cache use.
// request/take/cancel/stop do no native I/O or join. wake is coalesced until
// take(), may run on the worker, must marshal lifetime-checked UI work and
// outlive join(). Callback exceptions poison the worker. Destruction/join WAIT;
// the composition root must withdraw views and join outside UI dispatch.
class FilesCompletionService final {
  public:
    FilesCompletionService(FilesPaneBackend backend, FilesCompletionServiceLimits limits = {},
                           std::function<void()> wake = {});
    ~FilesCompletionService();
    FilesCompletionService(const FilesCompletionService &) = delete;
    FilesCompletionService &operator=(const FilesCompletionService &) = delete;
    // The caller supplies a nonzero field-local revision; a reused field may
    // restart its revisions, so the separate service ticket is also mandatory.
    // Queries are independently clamped/revalidated against service limits.
    FilesCompletionSubmitted request(FilesLocationCompletionQuery query, std::uint64_t revision,
                                     std::optional<bool> show_hidden = {},
                                     std::optional<FilesCompletionCursor> after = {}, bool refresh = false);
    bool cancel(const FilesCompletionTicket &request);
    std::optional<FilesCompletionReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;
    FilesPaneWorkerFault fault() const noexcept;

  private:
    struct State;
    static FilesCompletionTicket ticket(std::uint64_t number) { return FilesCompletionTicket(number); }
    std::unique_ptr<State> state_;
};
} // namespace ck::vision
