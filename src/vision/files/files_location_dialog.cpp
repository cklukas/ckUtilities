// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location_dialog.hpp"
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/label.hpp>
#include <stdexcept>
namespace ck::vision {
namespace {
std::string location_error(const ck::files::UnavailableObservation &error) {
    using namespace ck::files;
    if (error.error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit))
        return "This location exceeds a configured text, path or folder-depth limit.";
    if (error.error.code == static_cast<std::int64_t>(AdapterError::UnrepresentableName))
        return "These native names require the exact format.";
    return "Enter a valid absolute path or a complete exact-name location. Nothing has been changed.";
}
} // namespace
struct FilesLocationDialog::State : std::enable_shared_from_this<State> {
    ckv::ui::Application &app;
    FilesLocationDraft draft;
    Completion completion;
    ckv::widgets::DialogDescriptor descriptor;
    ckv::widgets::Window *window = nullptr;
    std::weak_ptr<void> window_lifetime;
    ckv::widgets::InputLine *field = nullptr;
    ckv::widgets::Label *feedback = nullptr;
    ckv::widgets::Button *switch_button = nullptr;
    std::optional<FilesPaneLocation> candidate, selected;
    bool accepting = false, finished = false, delivered = false;
    State(ckv::ui::Application &application, FilesLocationDraft value, Completion handler)
        : app(application), draft(std::move(value)), completion(std::move(handler)) {}
    bool exact() const noexcept { return draft.syntax() == FilesLocationTextSyntax::ExactNames; }
    void mode_labels() {
        if (feedback)
            feedback->set_text(exact() ? "Exact names: anchor, then children" : "Path: a new absolute location");
        if (switch_button)
            switch_button->set_text(exact() ? "Use &path" : "Use &exact names");
    }
    void change_format() {
        if (finished || !field)
            return;
        auto converted = draft.reformat(field->text(),
                                        exact() ? FilesLocationTextSyntax::Path : FilesLocationTextSyntax::ExactNames);
        if (!converted.value()) {
            feedback->set_text(converted.unavailable_detail()->error.code ==
                                       static_cast<std::int64_t>(ck::files::AdapterError::InventoryLimit)
                                   ? "Location format exceeds its limit."
                                   : "Finish this location or keep its format.");
            field->set_valid(false);
            app.set_focus(field);
            return;
        }
        draft = std::move(converted).take_value();
        candidate.reset();
        field->set_text(draft.text());
        field->set_valid(true);
        mode_labels();
        app.set_focus(field);
    }
    void present(ckv::widgets::Desktop &desktop, const ckv::ui::StandardRoles &roles) {
        using namespace ckv::widgets;
        const auto weak = weak_from_this();
        descriptor.title = "Go to location";
        descriptor.resizable = true;
        descriptor.field_description_rows = 2;
        descriptor.fields = {
            FieldDescriptor{.label = "&Location",
                            .initial_text = draft.text(),
                            .description = "Path is a new absolute address. Exact names use a quoted anchor followed "
                                           "by literal child names; escapes retain unusual native units."},
            FieldDescriptor{.label = "", .kind = FieldKind::Note}};
        descriptor.check = [weak](const DialogResult &result) -> std::optional<DialogVeto> {
            const auto self = weak.lock();
            if (!self || self->finished || result.values.size() != 2)
                return DialogVeto{"The location form is no longer available.", 0};
            self->candidate.reset();
            auto parsed = self->draft.resolve(result.values[0]);
            if (!parsed.value())
                return DialogVeto{location_error(*parsed.unavailable_detail()), 0};
            self->candidate = std::move(parsed).take_value();
            return std::nullopt;
        };
        descriptor.buttons = {{"&Go", ButtonRole::Accept,
                               [weak] {
                                   if (auto self = weak.lock())
                                       self->accepting = true;
                               }},
                              {"Use &exact names", ButtonRole::Neutral,
                               [weak] {
                                   if (auto self = weak.lock())
                                       self->change_format();
                               }},
                              {"&Cancel", ButtonRole::Dismiss, {}}};
        auto form = materialize_dialog(descriptor);
        field = form.inputs.at(0);
        feedback = form.labels.at(1);
        switch_button = form.buttons.at(1);
        auto *initial = form.initial_focus;
        auto owner = std::make_unique<Window>(descriptor.title);
        window = owner.get();
        window_lifetime = window->lifetime_token();
        owner->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame,
                                 roles.dialog_background);
        owner->set_content_margin(1, 1);
        const auto self = shared_from_this();
        owner->on_closed = [self] {
            if (self->accepting)
                self->selected = std::move(self->candidate);
            else
                self->selected.reset();
            if (self->window && !self->window_lifetime.expired())
                schedule_self_detach(*self->window, self->app);
        };
        wire_dialog_window(*owner, std::move(form), descriptor, app, app.focused());
        auto default_accept = owner->accept_request;
        owner->accept_request = [weak, accept = std::move(default_accept)] {
            const auto held = weak.lock();
            if (!held || held->finished)
                return;
            held->accepting = false;
            accept();
            held->accepting = false;
        };
        owner->on_detached = [self] { self->detached(); };
        field->on_edited = [weak] {
            if (auto held = weak.lock(); held && !held->finished) {
                held->candidate.reset();
                held->field->set_valid(true);
                held->mode_labels();
            }
        };
        mode_labels();
        if (!desktop.present_modal(WindowHandle{std::move(owner), initial}, app))
            detached();
    }
    void detached() {
        if (finished)
            return;
        finished = true;
        window = nullptr;
        field = nullptr;
        feedback = nullptr;
        switch_button = nullptr;
        if (!completion) {
            delivered = true;
            return;
        }
        app.post([self = shared_from_this()] {
            self->delivered = true;
            auto handler = std::move(self->completion);
            if (handler)
                handler(std::move(self->selected));
        });
    }
    void close() {
        if (window && !window_lifetime.expired())
            window->close();
    }
};
FilesLocationDialog::FilesLocationDialog(ckv::ui::Application &app, ckv::widgets::Desktop &desktop,
                                         const ckv::ui::StandardRoles &roles, FilesLocationDraft draft,
                                         Completion completion)
    : state_(std::make_shared<State>(app, std::move(draft), std::move(completion))) {
    state_->present(desktop, roles);
}
FilesLocationDialog::~FilesLocationDialog() {
    state_->completion = {};
    state_->selected.reset();
}
bool FilesLocationDialog::completed() const noexcept { return state_->delivered; }
void FilesLocationDialog::close() {
    state_->selected.reset();
    state_->accepting = false;
    state_->close();
}
ckv::widgets::InputLine *FilesLocationDialog::input() const noexcept {
    return state_->finished || state_->window_lifetime.expired() ? nullptr : state_->field;
}
} // namespace ck::vision
