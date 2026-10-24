// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_completion_service.hpp"
#include "files_pane_grid.hpp"
#include "files_pane_service.hpp"
#include "ck/vision/suite_shell.hpp"
#include <array>
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
// Real read-only two-pane presentation. The composition root exclusively lends
// two services and the standard Quit handler. It joins services OUTSIDE dispatch
// after this controller withdraws callbacks. The host callback is restored on
// destruction and must request outer-loop exit without joining workers itself.
class FilesApp final {
  public:
    FilesApp(ckv::ui::Application &application, std::array<FilesPaneService *, 2> services, FilesAppWake &wake,
             std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> host_quit,
             ThemeScheme theme = ThemeScheme::Classic,
             std::array<FilesCompletionService *, 2> completion_services = {});
    ~FilesApp();
    FilesApp(const FilesApp &) = delete;
    FilesApp &operator=(const FilesApp &) = delete;
    bool navigate(std::size_t pane, ck::files::NativeAbsolutePath location);
    // Submit the cached stamped directory row; all native checks stay on the worker.
    bool enter_directory(std::size_t pane);
    bool navigate_parent(std::size_t pane);
    bool navigate_root(std::size_t pane);
    bool navigate_home(std::size_t pane);
    bool navigate_history_back(std::size_t pane);
    bool navigate_history_forward(std::size_t pane);
    bool add_bookmark(std::size_t pane);
    bool remove_bookmark(std::size_t pane);
    bool navigate_bookmark(std::size_t pane, std::size_t bookmark);
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
    bool location_editor_pending() const;
    std::size_t history_size(std::size_t pane) const;
    std::size_t history_position(std::size_t pane) const;
    std::size_t bookmark_count(std::size_t pane) const;
    std::size_t active_pane() const noexcept;
    bool narrow() const noexcept;
    bool pending(std::size_t pane) const;
    std::string status(std::size_t pane) const;
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
