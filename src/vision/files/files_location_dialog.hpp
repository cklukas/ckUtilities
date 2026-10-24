// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_completion_service.hpp"
#include "files_location_draft.hpp"
#include <cvision/ui/application.hpp>
#include <cvision/ui/standard_roles.hpp>
#include <cvision/widgets/desktop.hpp>
#include <cvision/widgets/input_line.hpp>
namespace ck::vision {
// Public ckVision materialized form/window APIs only. No nested event loop,
// native discovery or worker join. Completion is posted after modal detachment;
// destruction withdraws it; the Desktop retains the still-live window, like a
// standard ckVision presentation. Application/Desktop outlive this controller.
class FilesLocationDialog final {
  public:
    using Completion = std::function<void(std::optional<FilesPaneLocation>)>;
    FilesLocationDialog(ckv::ui::Application &app, ckv::widgets::Desktop &desktop, const ckv::ui::StandardRoles &roles,
                        FilesLocationDraft draft, FilesCompletionService *completion_service, Completion completion);
    ~FilesLocationDialog();
    FilesLocationDialog(const FilesLocationDialog &) = delete;
    FilesLocationDialog &operator=(const FilesLocationDialog &) = delete;
    bool completed() const noexcept;
    void close();
    // Typed observer for an instance-owned completion provider. Null after
    // detachment; callers must still respect this dialog's owning UI thread.
    ckv::widgets::InputLine *input() const noexcept;
    // Delivers one worker-owned value reply on the UI thread. Replies for a
    // closed dialog, another ticket, or another field revision are ignored.
    void publish_completion(FilesCompletionReply reply);

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ck::vision
