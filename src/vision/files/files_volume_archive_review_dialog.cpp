// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive_review_dialog.hpp"

#include <cvision/widgets/dialog.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ck::vision {
namespace {
using namespace ck::files;
using namespace ckv::widgets;

std::string consistency(FilesVolumeArchiveConsistency value) {
    return value == FilesVolumeArchiveConsistency::Snapshot ? "Snapshot" : "Live read";
}

std::string loss_kind(FilesVolumeArchiveLossKind value) {
    switch (value) {
    case FilesVolumeArchiveLossKind::UnsupportedObject: return "unsupported object";
    case FilesVolumeArchiveLossKind::Metadata: return "metadata";
    case FilesVolumeArchiveLossKind::AdditionalChannel: return "additional data channel";
    case FilesVolumeArchiveLossKind::Security: return "security";
    case FilesVolumeArchiveLossKind::StorageRepresentation: return "storage representation";
    case FilesVolumeArchiveLossKind::Name: return "name";
    case FilesVolumeArchiveLossKind::Consistency: return "consistency";
    case FilesVolumeArchiveLossKind::Data: return "data";
    }
    return "unknown";
}

std::string bytes(std::uint64_t value) { return std::to_string(value) + " bytes"; }

std::string display_archive_path(const std::string &value, ck::files::NameEncoding encoding) {
    if (encoding == ck::files::NameEncoding::PosixBytes)
        return ck::files::display_native_text(value);
    if (encoding != ck::files::NameEncoding::WindowsUtf16 || value.size() % 2 != 0)
        return "<unrepresentable native path>";
    std::u16string units;
    units.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2)
        units.push_back(static_cast<char16_t>(static_cast<unsigned char>(value[i])) |
                        static_cast<char16_t>(static_cast<unsigned char>(value[i + 1])) << 8);
    return ck::files::display_native_text(units);
}

} // namespace

struct FilesVolumeArchiveReviewDialog::State final : std::enable_shared_from_this<State> {
    ckv::ui::Application &application;
    ckv::widgets::Desktop &desktop;
    const ckv::ui::StandardRoles &roles;
    FilesVolumeArchiveReview::Pointer review;
    Completion completion;
    DialogDescriptor descriptor;
    std::optional<DescriptorDialogPresentation> presentation;
    ckv::widgets::Window *window = nullptr;
    std::weak_ptr<void> window_lifetime;
    std::vector<std::size_t> loss_fields;
    std::size_t field_count = 0;
    bool finished = false;
    bool delivered = false;
    std::optional<FilesVolumeArchiveApproval> approval;

    State(ckv::ui::Application &app, ckv::widgets::Desktop &desk, const ckv::ui::StandardRoles &standard_roles,
          FilesVolumeArchiveReview::Pointer model, Completion handler)
        : application(app), desktop(desk), roles(standard_roles), review(std::move(model)),
          completion(std::move(handler)) {
        if (!review)
            throw std::invalid_argument("invalid volume archive review dialog");
    }

    void build_descriptor() {
        const auto &document = review->document();
        const auto &summary = review->summary();
        descriptor.title = "Review volume archive";
        descriptor.resizable = true;
        descriptor.field_description_rows = 2;
        descriptor.fields = {
            {.label = "Source: " + document.source.name + " | " + document.source.mount_point,
             .kind = FieldKind::Note},
            {.label = "Filesystem: " + document.source.filesystem + " | Identity: " + document.source.identity,
             .kind = FieldKind::Note},
            {.label = "Consistency: " + consistency(document.consistency) + " | Destination: " +
                           review->destination_directory().display_text(),
             .kind = FieldKind::Note},
            {.label = "Archive: " + review->archive_name(), .kind = FieldKind::Note},
            {.label = "Logical data: " + bytes(summary.logical_bytes) + " | Stored extents: " +
                           bytes(summary.data_bytes),
             .kind = FieldKind::Note},
            {.label = "Entries: " + std::to_string(summary.entries) + " | Folders: " +
                           std::to_string(summary.directories) + " | Files: " + std::to_string(summary.regular_files) +
                           " | Links: " + std::to_string(summary.links) + " | Hard links: " +
                           std::to_string(summary.hard_links) + " | Sparse files: " +
                           std::to_string(summary.sparse_files),
             .kind = FieldKind::Note}};
        if (summary.destination_conflict)
            descriptor.fields.push_back({.label = "The archive already exists. Choose another destination and start a new review.",
                                         .kind = FieldKind::Note});
        if (document.losses.empty()) {
            descriptor.fields.push_back({.label = "No unencoded losses were found.", .kind = FieldKind::Note});
        } else {
            descriptor.fields.push_back({.label = "Review each listed non-hard loss before continuing.",
                                         .kind = FieldKind::Note});
            for (const auto &loss : document.losses) {
                loss_fields.push_back(descriptor.fields.size());
                descriptor.fields.push_back({.label = display_archive_path(loss.path, document.path_encoding) + ": " +
                                                     loss_kind(loss.kind) + " — " +
                                                     loss.diagnostic + (loss.hard_failure ? " [cannot continue]" : ""),
                                             .kind = FieldKind::Check,
                                             .initial_checked = false});
            }
        }
        field_count = descriptor.fields.size();
        if (!summary.destination_conflict && summary.hard_losses == 0)
            descriptor.buttons = {{"&Create archive", ButtonRole::Accept, {}},
                                  {"&Cancel", ButtonRole::Dismiss, {}}};
        else
            descriptor.buttons = {{"&Cancel", ButtonRole::Dismiss, {}}};
        const auto weak = weak_from_this();
        descriptor.check = [weak](const DialogResult &result) -> std::optional<DialogVeto> {
            const auto self = weak.lock();
            if (!self || self->finished || result.values.size() != self->field_count ||
                result.checked.size() != self->field_count)
                return DialogVeto{"The volume archive review is no longer available.", 0};
            std::vector<std::size_t> reviewed;
            reviewed.reserve(self->loss_fields.size());
            for (std::size_t i = 0; i != self->loss_fields.size(); ++i)
                if (result.checked[self->loss_fields[i]])
                    reviewed.push_back(i);
            const auto decision = self->review->document().losses.empty()
                                      ? FilesVolumeArchiveReviewDecision::Proceed
                                      : FilesVolumeArchiveReviewDecision::ContinueWithListedLosses;
            auto approved = self->review->approve(decision, reviewed);
            if (!approved.value())
                return DialogVeto{"Every loss must be reviewed; hard losses cannot be continued.", 0};
            self->approval = std::move(approved).take_value();
            return std::nullopt;
        };
    }

    void present() {
        build_descriptor();
        presentation.emplace(present_modal_dialog(std::move(descriptor), application, desktop, roles));
        window = desktop.active_window();
        if (!window)
            throw std::logic_error("volume archive review did not create a window");
        window_lifetime = window->lifetime_token();
        const auto self = shared_from_this();
        presentation->set_completion_handler([self](DialogResult result) { self->complete(std::move(result)); });
    }

    void complete(DialogResult result) {
        if (finished)
            return;
        finished = true;
        window = nullptr;
        auto outcome = result.accepted && approval
                           ? Observation<FilesVolumeArchiveApproval>::available(std::move(*approval))
                           : review->approve(FilesVolumeArchiveReviewDecision::Cancel);
        approval.reset();
        auto callback = std::move(completion);
        if (!callback) {
            delivered = true;
            return;
        }
        application.post([self = shared_from_this(), delivered_callback = std::move(callback),
                             delivered_outcome = std::move(outcome)]() mutable {
            self->delivered = true;
            delivered_callback(std::move(delivered_outcome));
        });
    }

    void close() {
        if (window && !window_lifetime.expired())
            window->close();
    }
};

FilesVolumeArchiveReviewDialog::FilesVolumeArchiveReviewDialog(
    ckv::ui::Application &application, ckv::widgets::Desktop &desktop, const ckv::ui::StandardRoles &roles,
    FilesVolumeArchiveReview::Pointer review, Completion completion)
    : state_(std::make_shared<State>(application, desktop, roles, std::move(review), std::move(completion))) {
    state_->present();
}

FilesVolumeArchiveReviewDialog::~FilesVolumeArchiveReviewDialog() { state_->completion = {}; }
bool FilesVolumeArchiveReviewDialog::completed() const noexcept { return state_->delivered; }
void FilesVolumeArchiveReviewDialog::close() { state_->close(); }

} // namespace ck::vision
