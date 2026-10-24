// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
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
             ThemeScheme theme = ThemeScheme::Classic);
    ~FilesApp();
    FilesApp(const FilesApp &) = delete;
    FilesApp &operator=(const FilesApp &) = delete;
    bool navigate(std::size_t pane, ck::files::NativeAbsolutePath location);
    void switch_pane();
    void cancel_read();
    void request_close();
    bool close_confirmation_pending() const;
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
