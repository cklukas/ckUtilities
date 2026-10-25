// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_review.hpp"

#include <cvision/ui/application.hpp>
#include <cvision/ui/standard_roles.hpp>
#include <cvision/widgets/dialog.hpp>

#include <functional>
#include <memory>

namespace ck::vision {

// UI-only presentation of an immutable operation review. It never opens a
// native object or executes an operation; the completion carries only the
// plan-owned consent receipt (or a conservative Cancel receipt on dismissal).
class FilesOperationReviewDialog final {
  public:
    using Completion = std::function<void(ck::files::Observation<ck::files::ConsentReceipt>)>;

    FilesOperationReviewDialog(ckv::ui::Application &application, ckv::widgets::Desktop &desktop,
        const ckv::ui::StandardRoles &roles, ck::files::OperationReviewModel::Pointer model,
        ck::files::NameEncoding destination_encoding, Completion completion);
    ~FilesOperationReviewDialog();
    FilesOperationReviewDialog(const FilesOperationReviewDialog &) = delete;
    FilesOperationReviewDialog &operator=(const FilesOperationReviewDialog &) = delete;

    bool completed() const noexcept;
    void close();

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace ck::vision
