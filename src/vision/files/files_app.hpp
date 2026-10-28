// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_command_availability.hpp"
#include "files_completion_service.hpp"
#include "files_operation_planning_service.hpp"
#include "files_operation_execution_service.hpp"
#include "files_operation_review_dialog.hpp"
#include "files_volume_archive_review_dialog.hpp"
#include "files_settings.hpp"
#include "files_volume_service.hpp"
#include "files_pane_grid.hpp"
#include "files_pane_service.hpp"
#include "ck/vision/suite_shell.hpp"
#include <array>
#include <string_view>
namespace ck::vision {
// One payload-free queued UI callback for both panes. Construct before services;
// pass callback() to their wake parameter. Application outlives this bridge and
// joined services. Binding/unbinding is UI-only; request() is thread-safe.
class FilesAppWake final {
  public:
    explicit FilesAppWake(ckv::ui::Application &application);
    ~FilesAppWake();
    FilesAppWake(const FilesAppWake &) = delete;
    FilesAppWake &operator=(const FilesAppWake &) = delete;
    std::function<void()> callback() const;
    void request() const;
    void bind(std::function<void()> handler);
    void unbind() noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
struct FilesAppTarget {
    FilesPaneTicket navigation;
    FilesPaneRowTarget row;
};
// Real two-pane presentation. Read operations and mutation requests stay on
// workers; the composition root exclusively lends
// two services and the standard Quit handler. It joins services OUTSIDE dispatch
// after this controller withdraws callbacks. The host callback is restored on
// destruction and must request outer-loop exit without joining workers itself.
class FilesApp final {
  public:
    // The callback must only compare the receipt with already-published fresh
    // worker evidence; it must not perform native I/O or block the UI thread.
    using OperationRevalidation = std::function<ck::files::Observation<ck::files::ConsentReceipt>(
        const ck::files::ConsentReceipt &)>;
    FilesApp(ckv::ui::Application &application, std::array<FilesPaneService *, 2> services, FilesAppWake &wake,
             std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> host_quit,
             ThemeScheme theme = ThemeScheme::Classic,
             std::array<FilesCompletionService *, 2> completion_services = {},
             FilesOperationPlanningService *planning_service = nullptr,
             FilesOperationExecutionService *execution_service = nullptr,
             std::shared_ptr<const ck::files::NativeAbsolutePath> operation_journal_root = {},
             FilesSettingsBinding settings = {}, FilesVolumeService *volume_service = nullptr,
             FilesVolumeArchivePlanningService *volume_archive_planning_service = nullptr,
             FilesVolumeArchiveService *volume_archive_service = nullptr,
             FilesVolumeArchiveRecoveryService *volume_archive_recovery_service = nullptr);
    ~FilesApp();
    FilesApp(const FilesApp &) = delete;
    FilesApp &operator=(const FilesApp &) = delete;
    bool navigate(std::size_t pane, ck::files::NativeAbsolutePath location);
    // Submit the cached stamped directory row; all native checks stay on the worker.
    bool enter_directory(std::size_t pane);
    bool navigate_parent(std::size_t pane);
    bool navigate_root(std::size_t pane);
    bool navigate_home(std::size_t pane);
    bool refresh(std::size_t pane);
    bool navigate_history_back(std::size_t pane);
    bool navigate_history_forward(std::size_t pane);
    bool add_bookmark(std::size_t pane);
    bool remove_bookmark(std::size_t pane);
    bool navigate_bookmark(std::size_t pane, std::size_t bookmark);
    // Creates one exact native directory component relative to the pane's
    // currently acknowledged route, then refreshes only after worker success.
    bool create_directory(std::size_t pane, ck::files::NativeName name);
    // Renames the currently acknowledged target through the worker's native
    // identity protocol, then refreshes only after success.
    bool rename(std::size_t pane, ck::files::NativeName name);
    bool trash(std::size_t pane);
    bool permanently_delete(std::size_t pane);
    // Last acknowledged requested route, not current membership or operation authority.
    std::shared_ptr<const FilesPaneLocation> location(std::size_t pane) const;
    bool change_marks(std::size_t pane, ck::files::IndexedMarkCommand command);
    bool change_view(std::size_t pane, ck::files::PaneView view);
    const ck::files::PaneView &view(std::size_t pane) const;
    const ck::files::IndexedPaneSelection &selection(std::size_t pane) const;
    void switch_pane();
    void cancel_read();
    void request_close();
    bool close_confirmation_pending() const;
    bool view_options_pending() const;
    bool properties_pending() const;
    bool preview_pending() const;
    bool operation_review_pending() const;
    bool location_editor_pending() const;
    bool volume_info_pending() const;
    bool review_operation(ck::files::OperationReviewModel::Pointer model,
        ck::files::NameEncoding destination_encoding, OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion);
    bool prepare_operation(FilesOperationPlanningRequest request,
        ck::files::NameEncoding destination_encoding, OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion);
    bool prepare_operation(FilesOperationPanePlanningRequest request,
        ck::files::NameEncoding destination_encoding, OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion);
    std::size_t history_size(std::size_t pane) const;
    std::size_t history_position(std::size_t pane) const;
    std::size_t bookmark_count(std::size_t pane) const;
    std::size_t active_pane() const noexcept;
    bool narrow() const noexcept;
    bool pending(std::size_t pane) const;
    std::string status(std::size_t pane) const;
    FilesCommandAvailability availability(std::string_view command) const;
    std::optional<FilesAppTarget> target(std::size_t pane) const;
    const FilesPaneGridModel &model(std::size_t pane) const;
    ckv::widgets::CellGrid *grid(std::size_t pane) const;
    ckv::ui::CommandId switch_command() const noexcept;
    ckv::ui::CommandId cancel_command() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ck::vision
