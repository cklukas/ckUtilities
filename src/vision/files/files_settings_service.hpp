// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_settings.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace ck::vision {
// Same persisted-state limits as the version-1 codec. Admission checks exact
// encoded size BEFORE allocating/copying a snapshot; no bookmark is truncated.
struct FilesSettingsBounds {
    static constexpr std::size_t bytes = 1024 * 1024;
    static constexpr std::size_t bookmarks_per_pane = 256;
    static constexpr std::size_t filter_bytes = 4096;
    static constexpr std::size_t native_units_per_field = 256 * 1024;
};
// Borrowed only during the UI call. The worker never retains pointers/spans to
// panes or callbacks' stack data. Exact native routes are copied after checking.
struct FilesSettingsSnapshotView {
    std::array<const ck::files::PaneView *, 2> views{};
    std::array<std::span<const std::shared_ptr<const FilesPaneLocation>>, 2> bookmarks{};
};
enum class FilesSettingsAdmission { Accepted, Closed, Limit, Invalid, NoMemory, WorkerFault, RevisionExhausted };
struct FilesSettingsSubmitted {
    FilesSettingsAdmission state = FilesSettingsAdmission::Closed;
    std::uint64_t revision = 0;
};
enum class FilesSettingsWriteState { Saved, StoreError, NoMemory, WorkerFault };
// POD completion remains publishable/takeable under persistent allocation
// failure, including MSVC Debug iterator/proxy allocation failures.
struct FilesSettingsReply {
    std::uint64_t revision = 0;
    FilesSettingsWriteState state = FilesSettingsWriteState::WorkerFault;
    bool outcome_unknown = false;
    bool diagnostic_truncated = false;
    std::size_t diagnostic_size = 0;
    std::array<char, 4096> diagnostic{};
    std::string_view message() const noexcept { return {diagnostic.data(), diagnostic_size}; }
};
struct FilesSettingsProgress {
    std::uint64_t accepted_revision = 0, finished_revision = 0, saved_revision = 0;
    bool closing = false, drained = false, wake_failed = false, worker_fault = false;
};
std::string_view files_settings_admission_message(FilesSettingsAdmission) noexcept;
std::string_view files_settings_write_message(FilesSettingsWriteState) noexcept;
// Optionally include ONE prospective bookmark without changing the UI state.
FilesSettingsAdmission validate_files_settings_snapshot(const FilesSettingsSnapshotView &,
    std::size_t append_pane = 2, const FilesPaneLocation *append = nullptr) noexcept;

class FilesSettingsService final {
  public:
    // Startup load must finish before construction. Store and its filesystem
    // outlive this service; only this worker calls Store::save afterwards.
    // Wake must enqueue/signify UI work, never access UI state or destroy/join
    // this service. The main owner joins BEFORE destroying its wake/Application.
    explicit FilesSettingsService(FilesSettingsStore &, std::function<void()> wake = {});
    ~FilesSettingsService(); // Ordinary composition-owner join, outside dispatch.
    FilesSettingsService(const FilesSettingsService &) = delete;
    FilesSettingsService &operator=(const FilesSettingsService &) = delete;
    // UI-only admission, memory operations only. One active save plus one
    // coalesced latest pending snapshot; a failed admission preserves both.
    FilesSettingsSubmitted submit(const FilesPersistentSettings &) noexcept;
    FilesSettingsSubmitted submit(const FilesSettingsSnapshotView &) noexcept;
    // UI memory-only APIs. take uses try_lock; callbacks never wait for I/O.
    std::optional<FilesSettingsReply> take() noexcept;
    // Retained even after take(); owner can report final failure after drain.
    std::optional<FilesSettingsReply> latest_result() const noexcept;
    FilesSettingsProgress progress() const noexcept;
    // Seal admission, drain every active save and the LATEST accepted pending
    // snapshot. Does not cancel an atomic write or discard unsaved latest state.
    void request_close() noexcept;
    bool drained() const noexcept;
    void join(); // Composition owner ONLY, after application.run(), never a UI callback.
  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ck::vision
