// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_review_dialog.hpp"

#include "name_mapping.hpp"

#include <cvision/widgets/dialog.hpp>

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
    DialogDescriptor descriptor;
    std::optional<DescriptorDialogPresentation> presentation;
    ckv::widgets::Window *window = nullptr;
    std::weak_ptr<void> window_lifetime;
    std::vector<std::size_t> issue_fields;
    std::vector<std::size_t> name_fields;
    std::size_t action_field = 0;
    std::size_t field_count = 0;
    std::vector<ck::files::ConsentAction> actions;
    std::optional<ck::files::ConsentReceipt> pending_receipt;
    bool finished = false;
    bool delivered = false;

    State(ckv::ui::Application &app, ckv::widgets::Desktop &desk, const ckv::ui::StandardRoles &standard_roles,
        ck::files::OperationReviewModel::Pointer review, ck::files::NameEncoding encoding, Completion handler)
        : application(app), desktop(desk), roles(standard_roles), model(std::move(review)),
          destination_encoding(encoding), completion(std::move(handler)) {
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
        actions = {ConsentAction::Cancel, ConsentAction::ChooseAnotherDestination};
        const bool issues = !model->issue_rows().empty();
        const bool conflicts = !model->conflict_rows().empty();
        if (!issues && !conflicts) {
            actions.push_back(ConsentAction::Proceed);
        } else if (issues && !conflicts) {
            if (model->summary().uncertain == 0 && model->summary().hard_failures == 0)
                actions.push_back(ConsentAction::ContinueWithListedLosses);
            actions.push_back(ConsentAction::SkipAffectedItems);
        } else if (!issues && conflicts) {
            actions.push_back(ConsentAction::SkipConflict);
            actions.push_back(ConsentAction::KeepBoth);
            if (all_replaceable())
                actions.push_back(ConsentAction::Replace);
            if (all_mergeable())
                actions.push_back(ConsentAction::MergeDirectories);
        }
    }

    static std::string action_label(ConsentAction action) {
        switch (action) {
        case ConsentAction::Cancel: return "Cancel";
        case ConsentAction::ChooseAnotherDestination: return "Choose another destination";
        case ConsentAction::Proceed: return "Proceed";
        case ConsentAction::SkipAffectedItems: return "Skip affected items";
        case ConsentAction::ContinueWithListedLosses: return "Continue with listed losses";
        case ConsentAction::SkipConflict: return "Skip listed conflicts";
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
        if (model->issue_rows().size() && model->conflict_rows().size())
            descriptor.fields.push_back({.label = "Preservation and collision decisions are reviewed separately; choose a new destination first.",
                .kind = FieldKind::Note});
        for (const auto &row : model->issue_rows()) {
            issue_fields.push_back(descriptor.fields.size());
            const auto &issue = row.issue;
            descriptor.fields.push_back({.label = "Review issue " + std::to_string(row.ordinal + 1) + ": " +
                                             consequence(issue.consequence) + " — " + feature(issue.feature) +
                                             " (" + issue_state(issue.state) + ")" +
                                             (issue.hard_failure ? " [cannot be continued]" : ""),
                .kind = FieldKind::Check,
                .initial_checked = false});
        }
        for (const auto &row : model->conflict_rows()) {
            descriptor.fields.push_back({.label = "Conflict " + std::to_string(row.ordinal + 1) + ": " +
                                             conflict_kind(row.conflict.kind),
                .kind = FieldKind::Note});
            FieldDescriptor name;
            name.label = "Keep both name " + std::to_string(row.ordinal + 1);
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
            for (std::size_t index = 0; index < self->issue_fields.size(); ++index)
                if (result.checked[self->issue_fields[index]])
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
                    if (!parsed)
                        return DialogVeto{"Keep both requires a valid native basename for every conflict.", field};
                    names.push_back(std::move(*parsed));
                }
            } else {
                conflicts.clear();
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
    ck::files::NameEncoding destination_encoding, Completion completion)
    : state_(std::make_shared<State>(application, desktop, roles, std::move(model), destination_encoding,
                                     std::move(completion))) {
    state_->present();
}

FilesOperationReviewDialog::~FilesOperationReviewDialog() {
    state_->completion = {};
}

bool FilesOperationReviewDialog::completed() const noexcept { return state_->delivered; }
void FilesOperationReviewDialog::close() { state_->close(); }

} // namespace ck::vision
