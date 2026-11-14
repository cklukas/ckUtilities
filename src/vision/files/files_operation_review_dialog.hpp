// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_path.hpp"
#include "operation_review.hpp"

#include <cvision/ui/application.hpp>
#include <cvision/ui/standard_roles.hpp>
#include <cvision/widgets/dialog.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ck::vision {

struct FilesTransferReviewScope final {
    ck::files::NativeAbsolutePath source, destination;
    std::vector<ck::files::NativeName> names;
};

void add_files_transfer_scope(ckv::widgets::DialogDescriptor &descriptor, const FilesTransferReviewScope &scope,
    ckv::ui::Application &application, ckv::widgets::Desktop &desktop, const ckv::ui::StandardRoles &roles);
std::string files_review_literal(std::string_view text);

// UI-only presentation of an immutable operation review. It never opens a
// native object or executes an operation; the completion carries only the
// plan-owned consent receipt (or a conservative Cancel receipt on dismissal).
class FilesOperationReviewDialog final {
  public:
    using Completion = std::function<void(ck::files::Observation<ck::files::ConsentReceipt>)>;

    FilesOperationReviewDialog(ckv::ui::Application &application, ckv::widgets::Desktop &desktop,
        const ckv::ui::StandardRoles &roles, ck::files::OperationReviewModel::Pointer model,
        ck::files::NameEncoding destination_encoding, Completion completion,
        std::shared_ptr<const ck::files::FrozenInventory> inventory = {},
        std::function<bool(ck::files::ConsentAction)> supports_action = {},
        std::optional<FilesTransferReviewScope> scope = {});
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
