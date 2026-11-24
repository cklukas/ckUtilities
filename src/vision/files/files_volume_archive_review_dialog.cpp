// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive_review_dialog.hpp"
#include "files_operation_review_dialog.hpp"

#include <cvision/ui/layout.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/label.hpp>
#include <cvision/widgets/list_view.hpp>

#include <algorithm>
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

struct FilesVolumeArchiveReviewDialog::State final : ListModel, std::enable_shared_from_this<State> {
    ckv::ui::Application &application;
    ckv::widgets::Desktop &desktop;
    const ckv::ui::StandardRoles &roles;
    FilesVolumeArchiveReview::Pointer review;
    Completion completion;
    DialogDescriptor descriptor;
    std::optional<DescriptorDialogPresentation> presentation;
    ckv::widgets::Window *window = nullptr;
    std::weak_ptr<void> window_lifetime;
    Window *loss_window = nullptr;
    Window *detail_window = nullptr;
    std::weak_ptr<void> detail_lifetime;
    ListView *loss_list = nullptr;
    Label *loss_status = nullptr;
    Button *acknowledge = nullptr;
    std::vector<bool> reviewed;
    std::size_t remaining = 0;
    bool finished = false;
    bool delivered = false;
    std::optional<FilesVolumeArchiveApproval> approval;

    State(ckv::ui::Application &app, ckv::widgets::Desktop &desk, const ckv::ui::StandardRoles &standard_roles,
          FilesVolumeArchiveReview::Pointer model, Completion handler)
        : application(app), desktop(desk), roles(standard_roles), review(std::move(model)),
          completion(std::move(handler)) {
        if (!review)
            throw std::invalid_argument("invalid volume archive review dialog");
        remaining = review->losses().size();
        reviewed.resize(remaining);
    }

    std::size_t item_count() const override { return review->losses().size(); }
    std::optional<std::size_t> index_of(ListItemId id) const override {
        if (id == 0 || id > item_count())
            return std::nullopt;
        return static_cast<std::size_t>(id - 1);
    }
    std::string loss_text(std::size_t index) const {
        const auto &loss = review->losses()[index];
        return display_archive_path(loss.path, review->path_encoding()) + ": " + loss_kind(loss.kind) +
               " — " + loss.diagnostic + (loss.hard_failure ? " [cannot continue]" : "");
    }
    ListItem item_at(std::size_t index) const override {
        const auto &loss = review->losses()[index];
        const auto prefix = loss.hard_failure ? "[!] " : reviewed[index] ? "[x] " : "[ ] ";
        return {index + 1, prefix + loss_text(index)};
    }
    void update_loss_controls() {
        const auto index = static_cast<std::size_t>(loss_list->cursor());
        const bool selected = index < item_count();
        acknowledge->set_enabled(selected && !review->losses()[index].hard_failure);
        acknowledge->set_text(selected && reviewed[index] ? "Undo &acknowledgement" : "&Acknowledge");
        loss_status->set_text(std::to_string(item_count() - remaining) + " of " +
                              std::to_string(item_count()) + " losses acknowledged");
    }
    void toggle_loss() {
        const auto index = static_cast<std::size_t>(loss_list->cursor());
        if (index >= item_count() || review->losses()[index].hard_failure)
            return;
        reviewed[index] = !reviewed[index];
        if (reviewed[index])
            --remaining;
        else
            ++remaining;
        loss_list->invalidate();
        update_loss_controls();
    }
    void show_loss_detail(std::size_t index) {
        detail_window = present_files_read_only_text("Archive loss details", loss_text(index), application,
                                                    desktop, roles);
        if (detail_window)
            detail_lifetime = detail_window->lifetime_token();
    }
    void show_losses() {
        if (loss_window || finished)
            return;
        auto owner = std::make_unique<Window>("Archive losses");
        const auto area = desktop.content_area();
        const int width = std::min(86, area.width), height = std::min(20, area.height);
        owner->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
        owner->set_content_margin(1, 1);
        owner->set_min_size({24, 6});
        owner->set_resizable(true);
        owner->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame,
                                 roles.dialog_background);
        auto content = std::make_unique<ckv::ui::Column>();
        auto status = std::make_unique<Label>("");
        loss_status = status.get();
        content->add_item(std::move(status), {.policy = ckv::ui::SizePolicy::Fixed});
        auto list = std::make_unique<ListView>();
        loss_list = list.get();
        list->set_model(*this);
        list->on_cursor_changed = [this](std::size_t) { update_loss_controls(); };
        list->on_activate = [this](std::size_t index) { show_loss_detail(index); };
        content->add_item(std::move(list), {.policy = ckv::ui::SizePolicy::Expanding});
        auto actions = std::make_unique<ckv::ui::Row>();
        auto mark = std::make_unique<Button>("&Acknowledge");
        acknowledge = mark.get();
        mark->on_press = [this] { toggle_loss(); };
        actions->add_item(std::move(mark), {.policy = ckv::ui::SizePolicy::Fixed});
        auto inspect = std::make_unique<Button>("&Details");
        inspect->on_press = [this] {
            if (const auto id = loss_list->cursor_id())
                if (const auto index = index_of(*id))
                    show_loss_detail(*index);
        };
        actions->add_item(std::move(inspect), {.policy = ckv::ui::SizePolicy::Fixed});
        auto back = std::make_unique<Button>("&Back");
        auto *child = owner.get();
        owner->cancel_request = [child] { child->close(); };
        back->on_press = owner->cancel_request;
        actions->add_item(std::move(back), {.policy = ckv::ui::SizePolicy::Fixed});
        content->add_item(std::move(actions), {.policy = ckv::ui::SizePolicy::Fixed});
        update_loss_controls();
        owner->set_content(std::move(content));
        const auto self = shared_from_this();
        owner->on_detached = [self] {
            self->loss_window = nullptr;
            self->loss_list = nullptr;
            self->loss_status = nullptr;
            self->acknowledge = nullptr;
        };
        loss_window = child;
        if (!desktop.present_modal(WindowHandle{std::move(owner), loss_list}, application))
            loss_window = nullptr;
    }

    void build_descriptor() {
        const auto &source = review->source();
        const auto &summary = review->summary();
        descriptor.title = "Review volume archive";
        descriptor.resizable = true;
        descriptor.field_description_rows = 2;
        descriptor.fields = {
            {.label = "Source: " + source.name + " | " + source.mount_point,
             .kind = FieldKind::Note},
            {.label = "Filesystem: " + source.filesystem + " | Identity: " + source.identity,
             .kind = FieldKind::Note},
            {.label = "Consistency: " + consistency(review->consistency()) + " | Destination: " +
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
        const auto weak = weak_from_this();
        if (review->losses().empty()) {
            descriptor.fields.push_back({.label = "No unencoded losses were found.", .kind = FieldKind::Note});
        } else {
            const auto instruction = summary.hard_losses
                ? std::to_string(summary.hard_losses) + " prevent archive creation."
                : "Acknowledge each loss before creating the archive.";
            descriptor.fields.push_back({.label = std::to_string(summary.losses) + " losses. " + instruction,
                                         .kind = FieldKind::Note});
            descriptor.buttons.push_back({"Review &losses", ButtonRole::Neutral, [weak] {
                if (const auto self = weak.lock())
                    self->show_losses();
            }});
        }
        if (!summary.destination_conflict && summary.hard_losses == 0)
            descriptor.buttons.push_back({"&Create archive", ButtonRole::Accept, {}});
        descriptor.buttons.push_back({"Ca&ncel", ButtonRole::Dismiss, {}});
        for (auto &field : descriptor.fields)
            field.label = files_review_literal(field.label);
        descriptor.check = [weak](const DialogResult &) -> std::optional<DialogVeto> {
            const auto self = weak.lock();
            if (!self || self->finished)
                return DialogVeto{"The volume archive review is no longer available.", 0};
            if (self->remaining)
                return DialogVeto{"Open Review losses and acknowledge each listed loss to continue.", {}};
            const auto decision = !self->review->requires_loss_consent()
                                      ? FilesVolumeArchiveReviewDecision::Proceed
                                      : FilesVolumeArchiveReviewDecision::ContinueWithListedLosses;
            auto approved = self->review->approve(decision, self->reviewed);
            if (!approved.value())
                return DialogVeto{"Every loss must be reviewed; hard losses cannot be continued.", 0};
            self->approval = std::move(approved).take_value();
            return std::nullopt;
        };
    }

    void present() {
        build_descriptor();
        presentation.emplace(present_modal_dialog(std::move(descriptor), application, desktop));
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
        if (detail_window && !detail_lifetime.expired())
            detail_window->close();
        if (loss_window)
            loss_window->close();
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
