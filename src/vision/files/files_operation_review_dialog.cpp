// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_review_dialog.hpp"

#include "frozen_inventory.hpp"
#include "name_mapping.hpp"
#include "windows_name.hpp"

#include <cvision/widgets/dialog.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ck::vision {
namespace {
using namespace ck::files;
using namespace ckv::widgets;

std::string feature(PreservationFeature value) {
    switch (value) {
    case PreservationFeature::MainData: return "main data";
    case PreservationFeature::AdditionalChannels: return "additional data channels";
    case PreservationFeature::ExtendedAttributes: return "extended attributes";
    case PreservationFeature::PermissionsOwnership: return "permissions and ownership";
    case PreservationFeature::TimesAndFlags: return "timestamps and flags";
    case PreservationFeature::LinksAndIdentity: return "links and identity";
    case PreservationFeature::StorageRepresentation: return "storage representation";
    case PreservationFeature::NamesAndStructure: return "names and structure";
    case PreservationFeature::ProviderManagedObject: return "provider-managed data";
    }
    return "unknown preservation feature";
}

std::string issue_state(PreservationIssueState value) {
    switch (value) {
    case PreservationIssueState::Missing: return "missing at destination";
    case PreservationIssueState::Unknown: return "unknown";
    case PreservationIssueState::Unsupported: return "unsupported";
    case PreservationIssueState::Denied: return "access denied";
    case PreservationIssueState::Changed: return "changed";
    case PreservationIssueState::Conflict: return "conflict";
    }
    return "unknown state";
}

std::string consequence(PreservationConsequence value) {
    switch (value) {
    case PreservationConsequence::DataLoss: return "data loss";
    case PreservationConsequence::SecurityChange: return "security change";
    case PreservationConsequence::MetadataLoss: return "metadata loss";
    case PreservationConsequence::RepresentationChange: return "representation change";
    case PreservationConsequence::Uncertain: return "uncertain result";
    }
    return "uncertain result";
}

std::string conflict_kind(DestinationConflictKind value) {
    switch (value) {
    case DestinationConflictKind::ExistingFile: return "existing file";
    case DestinationConflictKind::ExistingDirectory: return "existing directory";
    case DestinationConflictKind::TypeMismatch: return "file/directory type mismatch";
    case DestinationConflictKind::CaseCollision: return "name collision";
    case DestinationConflictKind::Unknown: return "unknown destination conflict";
    }
    return "unknown destination conflict";
}

std::string summary_text(const OperationReviewSummary &summary) {
    return "Review required — data loss: " + std::to_string(summary.data_loss) +
        ", security changes: " + std::to_string(summary.security_change) +
        ", metadata loss: " + std::to_string(summary.metadata_loss) +
        ", representation changes: " + std::to_string(summary.representation_change) +
        ", uncertain: " + std::to_string(summary.uncertain) +
        ", conflicts: " + std::to_string(summary.conflicts) + ".";
}

std::string bind_error(const UnavailableObservation &detail) {
    if (detail.error.code == static_cast<std::int64_t>(AdapterError::ProviderReviewRequired))
        return "Review every required row and resolve every listed conflict before continuing.";
    if (detail.error.code == static_cast<std::int64_t>(AdapterError::PlanDecisionStale))
        return "The operation changed while it was being reviewed. Start a new review.";
    return "This review decision is not valid for the current operation.";
}

std::optional<NativeName> parse_name(std::string text, NameEncoding encoding) {
    auto posix = NativeName::from_posix(std::move(text));
    if (!posix)
        return std::nullopt;
    if (encoding == NameEncoding::PosixBytes)
        return std::move(*posix);
    if (encoding != NameEncoding::WindowsUtf16)
        return std::nullopt;
    auto mapped = map_native_name(*posix, NameEncoding::WindowsUtf16, {});
    if (!mapped.value())
        return std::nullopt;
    return std::move(mapped).take_value();
}

} // namespace

struct FilesOperationReviewDialog::State final : std::enable_shared_from_this<State> {
    ckv::ui::Application &application;
    ckv::widgets::Desktop &desktop;
    const ckv::ui::StandardRoles &roles;
    ck::files::OperationReviewModel::Pointer model;
    ck::files::NameEncoding destination_encoding;
    Completion completion;
    ck::files::FrozenInventory::Pointer inventory;
    std::function<bool(ck::files::ConsentAction)> supports_action;
    DialogDescriptor descriptor;
    std::optional<DescriptorDialogPresentation> presentation;
    ckv::widgets::Window *window = nullptr;
    std::weak_ptr<void> window_lifetime;
    std::vector<std::size_t> issue_fields;
    std::vector<std::size_t> name_fields;
    std::optional<std::size_t> review_all_field;
    std::size_t action_field = 0;
    std::size_t field_count = 0;
    std::vector<ck::files::ConsentAction> actions;
    std::optional<ck::files::ConsentReceipt> pending_receipt;
    bool finished = false;
    bool delivered = false;

    State(ckv::ui::Application &app, ckv::widgets::Desktop &desk, const ckv::ui::StandardRoles &standard_roles,
        ck::files::OperationReviewModel::Pointer review, ck::files::NameEncoding encoding, Completion handler,
        ck::files::FrozenInventory::Pointer review_inventory, std::function<bool(ConsentAction)> supported)
        : application(app), desktop(desk), roles(standard_roles), model(std::move(review)),
          destination_encoding(encoding), completion(std::move(handler)), inventory(std::move(review_inventory)),
          supports_action(std::move(supported)) {
        if (!model || (destination_encoding != ck::files::NameEncoding::PosixBytes &&
                       destination_encoding != ck::files::NameEncoding::WindowsUtf16))
            throw std::invalid_argument("invalid Files operation review dialog");
    }

    bool all_replaceable() const noexcept {
        for (const auto &row : model->conflict_rows()) {
            const auto &conflict = row.conflict;
            if (conflict.kind != DestinationConflictKind::ExistingFile ||
                model->plan()->items()[conflict.item].kind == ObjectKind::Directory)
                return false;
        }
        return !model->conflict_rows().empty();
    }

    bool all_mergeable() const noexcept {
        for (const auto &row : model->conflict_rows()) {
            const auto &conflict = row.conflict;
            if (conflict.kind != DestinationConflictKind::ExistingDirectory ||
                model->plan()->items()[conflict.item].kind != ObjectKind::Directory)
                return false;
        }
        return !model->conflict_rows().empty();
    }

    void choose_actions() {
        actions = {ConsentAction::Cancel};
        if (model->plan()->requires_review())
            actions.push_back(ConsentAction::ChooseAnotherDestination);
        const bool issues = !model->issue_rows().empty();
        const bool conflicts = !model->conflict_rows().empty();
        if (!issues && !conflicts) {
            actions.push_back(ConsentAction::Proceed);
        } else if (issues && !conflicts) {
            if (model->plan()->continue_with_listed_losses_allowed())
                actions.push_back(ConsentAction::ContinueWithListedLosses);
            actions.push_back(ConsentAction::SkipAffectedItems);
        } else if (!issues && conflicts) {
            actions.push_back(ConsentAction::SkipConflict);
            actions.push_back(ConsentAction::KeepBoth);
            if (all_replaceable())
                actions.push_back(ConsentAction::Replace);
            if (all_mergeable())
                actions.push_back(ConsentAction::MergeDirectories);
        } else if (supports_action && conflicts) {
            actions.push_back(ConsentAction::SkipConflict);
            actions.push_back(ConsentAction::KeepBoth);
        }
        if (supports_action)
            std::erase_if(actions, [this](const auto action) { return !supports_action(action); });
    }

    std::string item_label(std::size_t item) const {
        if (!inventory || item >= inventory->entries().size())
            return "item " + std::to_string(item + 1);
        const auto& path = inventory->entries()[item].source_path;
        const auto parent = inventory->mount_policy() == InventoryMountPolicy::DirectObjects
            ? path.parent_address() : inventory->entries().front().source_path.parent_address();
        if (!parent.value())
            return path.display_text();
        const auto relative = path.relative_components_from(*parent.value());
        if (!relative.value() || relative.value()->empty())
            return path.display_text();
        std::string label;
        for (const auto& component : *relative.value()) {
            if (!label.empty()) label += '/';
            label += component.display_name();
        }
        return label;
    }

    static std::string action_label(ConsentAction action) {
        switch (action) {
        case ConsentAction::Cancel: return "Cancel";
        case ConsentAction::ChooseAnotherDestination: return "Choose another destination";
        case ConsentAction::Proceed: return "Proceed";
        case ConsentAction::SkipAffectedItems: return "Skip affected items";
        case ConsentAction::ContinueWithListedLosses: return "Continue with listed losses";
        case ConsentAction::SkipConflict: return "Keep existing targets and skip conflicts";
        case ConsentAction::KeepBoth: return "Keep both with reviewed names";
        case ConsentAction::Replace: return "Replace existing files";
        case ConsentAction::MergeDirectories: return "Merge existing directories";
        }
        return "Cancel";
    }

    void build_descriptor() {
        choose_actions();
        descriptor.title = "Review file operation";
        descriptor.resizable = true;
        descriptor.field_description_rows = 2;
        descriptor.fields.push_back({.label = summary_text(model->summary()), .kind = FieldKind::Note});
        const bool keep_both = std::find(actions.begin(), actions.end(), ConsentAction::KeepBoth) != actions.end();
        if (supports_action && !model->conflict_rows().empty()) {
            const bool skip_supported = std::find(actions.begin(), actions.end(), ConsentAction::SkipConflict) != actions.end();
            descriptor.fields.push_back({.label = keep_both
                ? "To keep both, enter a new name for each incoming file and review the losses."
                : skip_supported
                ? "Conflicting items stay where they are. Review losses for the other items."
                : "Choose another destination to continue this transfer.",
                .kind = FieldKind::Note});
        }
        for (const auto &row : model->issue_rows()) {
            issue_fields.push_back(descriptor.fields.size());
            const auto &issue = row.issue;
            descriptor.fields.push_back({.label = item_label(issue.item) + " · " +
                                             consequence(issue.consequence) + " — " + feature(issue.feature) +
                                             " (" + issue_state(issue.state) + ")" +
                                             (issue.hard_failure ? " [cannot be continued]" : ""),
                .kind = FieldKind::Check,
                .initial_checked = false});
        }
        if (issue_fields.size() > 1) {
            review_all_field = descriptor.fields.size();
            descriptor.fields.push_back({.label = "I reviewed all listed preservation losses", .kind = FieldKind::Check});
        }
        for (const auto &row : model->conflict_rows()) {
            descriptor.fields.push_back({.label = item_label(row.conflict.item) + " · " +
                                             conflict_kind(row.conflict.kind),
                .kind = FieldKind::Note});
            if (!keep_both)
                continue;
            FieldDescriptor name;
            name.label = "New name for " + item_label(row.conflict.item);
            name.validate = [](const std::string &value) { return value.size() <= 64 * 1024; };
            name.description = "Used only with Keep both. Enter one native basename, never a path.";
            descriptor.fields.push_back(std::move(name));
            name_fields.push_back(descriptor.fields.size() - 1);
        }
        action_field = descriptor.fields.size();
        std::vector<std::string> options;
        options.reserve(actions.size());
        for (const auto action : actions)
            options.push_back(action_label(action));
        descriptor.fields.push_back({.label = "&Decision", .kind = FieldKind::Combo,
            .options = std::move(options), .initial_selection = 0});
        field_count = descriptor.fields.size();
        descriptor.buttons = {{"&Apply decision", ButtonRole::Accept, {}}, {"&Cancel", ButtonRole::Dismiss, {}}};
        const auto weak = weak_from_this();
        descriptor.check = [weak](const DialogResult &result) -> std::optional<DialogVeto> {
            const auto self = weak.lock();
            if (!self || self->finished || result.values.size() != self->field_count ||
                result.checked.size() != self->field_count || result.selected.size() != self->field_count)
                return DialogVeto{"The operation review is no longer available.", 0};
            self->pending_receipt.reset();
            if (result.selected[self->action_field] < 0 ||
                static_cast<std::size_t>(result.selected[self->action_field]) >= self->actions.size())
                return DialogVeto{"Choose a decision.", self->action_field};
            const auto action = self->actions[static_cast<std::size_t>(result.selected[self->action_field])];
            std::vector<std::size_t> reviewed;
            reviewed.reserve(self->issue_fields.size());
            const bool review_all = self->review_all_field && result.checked[*self->review_all_field];
            for (std::size_t index = 0; index < self->issue_fields.size(); ++index)
                if (review_all || result.checked[self->issue_fields[index]])
                    reviewed.push_back(index);
            std::vector<std::size_t> conflicts;
            const auto conflict_action = action == ConsentAction::SkipConflict || action == ConsentAction::KeepBoth ||
                action == ConsentAction::Replace || action == ConsentAction::MergeDirectories;
            if (conflict_action)
                for (std::size_t index = 0; index < self->model->conflict_rows().size(); ++index)
                    conflicts.push_back(index);
            std::vector<NativeName> names;
            if (action == ConsentAction::KeepBoth) {
                for (const auto field : self->name_fields) {
                    auto parsed = parse_name(result.values[field], self->destination_encoding);
                    if (!parsed || (parsed->windows_units() &&
                                    !check_win32_component(*parsed->windows_units()).ordinary_component()))
                        return DialogVeto{"Keep both requires a valid native basename for every conflict.", field};
                    if (std::find(names.begin(), names.end(), *parsed) != names.end())
                        return DialogVeto{"Use a different name for each incoming file.", field};
                    names.push_back(std::move(*parsed));
                }
            }
            auto bound = self->model->bind(action, std::move(reviewed), std::move(conflicts), std::move(names));
            if (!bound.value())
                return DialogVeto{bind_error(*bound.unavailable_detail()), self->action_field};
            self->pending_receipt = std::move(bound).take_value();
            return std::nullopt;
        };
    }

    void present() {
        build_descriptor();
        presentation.emplace(present_modal_dialog(std::move(descriptor), application, desktop, roles));
        window = desktop.active_window();
        if (!window)
            throw std::logic_error("Files operation review did not create a window");
        window_lifetime = window->lifetime_token();
        const auto self = shared_from_this();
        presentation->set_completion_handler([self](DialogResult result) { self->complete(std::move(result)); });
    }

    void complete(DialogResult result) {
        if (finished)
            return;
        finished = true;
        window = nullptr;
        Observation<ConsentReceipt> outcome;
        if (result.accepted && pending_receipt)
            outcome = Observation<ConsentReceipt>::available(std::move(*pending_receipt));
        else
            outcome = model->bind(ConsentAction::Cancel);
        pending_receipt.reset();
        auto delivered_callback = std::move(completion);
        if (!delivered_callback) {
            delivered = true;
            return;
        }
        application.post([self = shared_from_this(), callback = std::move(delivered_callback),
                             consent = std::move(outcome)]() mutable {
            self->delivered = true;
            callback(std::move(consent));
        });
    }

    void close() {
        if (window && !window_lifetime.expired())
            window->close();
    }
};

FilesOperationReviewDialog::FilesOperationReviewDialog(ckv::ui::Application &application, ckv::widgets::Desktop &desktop,
    const ckv::ui::StandardRoles &roles, ck::files::OperationReviewModel::Pointer model,
    ck::files::NameEncoding destination_encoding, Completion completion,
    ck::files::FrozenInventory::Pointer inventory, std::function<bool(ConsentAction)> supports_action)
    : state_(std::make_shared<State>(application, desktop, roles, std::move(model), destination_encoding,
                                     std::move(completion), std::move(inventory), std::move(supports_action))) {
    state_->present();
}

FilesOperationReviewDialog::~FilesOperationReviewDialog() {
    state_->completion = {};
}

bool FilesOperationReviewDialog::completed() const noexcept { return state_->delivered; }
void FilesOperationReviewDialog::close() { state_->close(); }

} // namespace ck::vision
