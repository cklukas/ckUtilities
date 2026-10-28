// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_volume_archive.hpp"

#include <cvision/ui/application.hpp>
#include <cvision/ui/standard_roles.hpp>
#include <cvision/widgets/dialog.hpp>

#include <functional>
#include <memory>

namespace ck::vision {

// UI-only presentation of a worker-built volume archive review. It has no
// native handles and cannot start writing until the immutable review is
// explicitly approved.
class FilesVolumeArchiveReviewDialog final {
  public:
    using Completion = std::function<void(ck::files::Observation<FilesVolumeArchiveApproval>)>;

    FilesVolumeArchiveReviewDialog(ckv::ui::Application &application, ckv::widgets::Desktop &desktop,
                                   const ckv::ui::StandardRoles &roles,
                                   FilesVolumeArchiveReview::Pointer review, Completion completion);
    ~FilesVolumeArchiveReviewDialog();
    FilesVolumeArchiveReviewDialog(const FilesVolumeArchiveReviewDialog &) = delete;
    FilesVolumeArchiveReviewDialog &operator=(const FilesVolumeArchiveReviewDialog &) = delete;

    bool completed() const noexcept;
    void close();

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace ck::vision
