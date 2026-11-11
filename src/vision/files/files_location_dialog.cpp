// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_location_dialog.hpp"
#include <cvision/core/text.hpp>
#include <cvision/widgets/completion.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/label.hpp>
#include <stdexcept>
#include <string_view>
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

// Never copy a native diagnostic/path into the dialog. These fixed hints also
// cover malformed or oversized observations without interpreting their text.
std::string_view completion_failure_hint(const FilesCompletionReply &reply) noexcept {
    if (reply.state == FilesCompletionReplyState::LimitExceeded)
        return "Completion limit reached; narrow the location.";
    if (reply.state == FilesCompletionReplyState::WorkerFault)
        return "Folder completion unavailable; enter a full location.";
    if (reply.state == FilesCompletionReplyState::Cancelled)
        return "Folder completion cancelled; edit to retry.";
    if (reply.state == FilesCompletionReplyState::Stale)
        return "Folder completion changed; edit to retry.";
    if (reply.error && reply.error->state == ck::files::ObservationState::Denied)
        return "Folder completion denied; edit to retry.";
    return "Folder completion unavailable; edit to retry.";
}

class FilesLocationCompletionProvider final : public ckv::widgets::CompletionProvider {
  public:
    FilesLocationCompletionProvider(FilesCompletionService &service, ck::files::NameEncoding encoding,
                                    FilesLocationTextSyntax syntax, FilesLocationTextLimits limits,
                                    std::function<void()> feedback_changed)
        : service_(service), encoding_(encoding), syntax_(syntax), limits_(limits),
          feedback_changed_(std::move(feedback_changed)) {}

    void request(const ckv::widgets::CompletionRequest &request) override {
        query_ = request;
        snapshot_ = {request.revision, ckv::widgets::CompletionState::Pending, {}};
        set_hint({});
        if (!request.revision || request.has_selection || request.cursor != ckv::text::split_graphemes(request.text).size()) {
            cancel();
            snapshot_.state = ckv::widgets::CompletionState::Ready;
            return;
        }
        auto parsed = parse_files_location_completion(request.text, syntax_, encoding_, limits_);
        if (!parsed.value()) {
            cancel();
            snapshot_.state = ckv::widgets::CompletionState::Failed;
            set_hint("Completion needs a valid location.");
            return;
        }
        const auto submitted = service_.request(std::move(parsed).take_value(), request.revision, true);
        if (submitted.state != FilesCompletionSubmission::Accepted) {
            cancel();
            snapshot_.state = ckv::widgets::CompletionState::Failed;
            set_hint(submitted.state == FilesCompletionSubmission::LimitExceeded
                         ? "Completion limit reached; narrow the location."
                         : "Folder completion unavailable; enter a full location.");
            return;
        }
        ticket_ = submitted.ticket;
    }

    const ckv::widgets::CompletionSnapshot &snapshot() const noexcept override { return snapshot_; }

    void publish(FilesCompletionReply reply) {
        if (!ticket_.valid() || reply.request != ticket_ || reply.revision != query_.revision)
            return;
        ticket_ = {};
        snapshot_ = {query_.revision, ckv::widgets::CompletionState::Failed, {}};
        if (reply.state != FilesCompletionReplyState::Ready || !reply.page) {
            set_hint(completion_failure_hint(reply));
            return;
        }
        set_hint({});
        const auto graphemes = ckv::text::split_graphemes(query_.text);
        snapshot_.candidates.reserve(reply.page->entries.size());
        for (const auto &entry : reply.page->entries) {
            if (!entry.row || entry.text.empty())
                continue;
            snapshot_.candidates.push_back({entry.row, 0, graphemes.size(), entry.text});
        }
        snapshot_.state = reply.page->next || !reply.page->complete() ? ckv::widgets::CompletionState::Partial
                                                                        : ckv::widgets::CompletionState::Ready;
    }

    std::string_view hint() const noexcept { return hint_; }

    void cancel() {
        if (ticket_.valid())
            service_.cancel(ticket_);
        ticket_ = {};
    }

  private:
    void set_hint(std::string_view hint) {
        if (hint_ == hint)
            return;
        hint_ = hint; // Only static literals/empty views are admitted here.
        if (feedback_changed_)
            feedback_changed_(); // UI-only label update; never calls the field.
    }
    FilesCompletionService &service_;
    ck::files::NameEncoding encoding_;
    FilesLocationTextSyntax syntax_;
    FilesLocationTextLimits limits_;
    ckv::widgets::CompletionRequest query_;
    FilesCompletionTicket ticket_;
    ckv::widgets::CompletionSnapshot snapshot_;
    std::string_view hint_;
    std::function<void()> feedback_changed_;
};
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
    std::shared_ptr<FilesLocationCompletionProvider> completion_provider;
    FilesCompletionService *completion_service = nullptr;
    std::optional<FilesPaneLocation> candidate, selected;
    bool accepting = false, finished = false, delivered = false;
    State(ckv::ui::Application &application, FilesLocationDraft value, FilesCompletionService *service,
          Completion handler)
        : app(application), draft(std::move(value)), completion(std::move(handler)), completion_service(service) {}
    bool exact() const noexcept { return draft.syntax() == FilesLocationTextSyntax::ExactNames; }
    void install_completion_provider() {
        if (!completion_service || !field)
            return;
        completion_provider = std::make_shared<FilesLocationCompletionProvider>(
            *completion_service, draft.encoding(), draft.syntax(), draft.limits(), [weak = weak_from_this()] {
                if (auto self = weak.lock(); self && !self->finished)
                    self->mode_labels();
            });
        field->set_completion_provider(completion_provider);
    }
    void mode_labels() {
        if (feedback) {
            const auto hint = completion_provider ? completion_provider->hint() : std::string_view{};
            feedback->set_text(!hint.empty() ? std::string(hint)
                                           : exact() ? "Exact names: anchor, then children" : "Path: a new absolute location");
        }
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
        if (completion_provider) {
            completion_provider->cancel();
            completion_provider.reset();
            field->set_completion_provider(nullptr);
        }
        field->set_text(draft.text());
        install_completion_provider();
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
        install_completion_provider();
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
        if (completion_provider)
            completion_provider->cancel();
        if (field)
            field->set_completion_provider(nullptr);
        completion_provider.reset();
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
        if (completion_provider)
            completion_provider->cancel();
        if (window && !window_lifetime.expired())
            window->close();
    }
    void publish_completion(FilesCompletionReply reply) {
        if (finished || !completion_provider || !field)
            return;
        completion_provider->publish(std::move(reply));
        field->completion_changed();
    }
};
FilesLocationDialog::FilesLocationDialog(ckv::ui::Application &app, ckv::widgets::Desktop &desktop,
                                         const ckv::ui::StandardRoles &roles, FilesLocationDraft draft,
                                         FilesCompletionService *completion_service, Completion completion)
    : state_(std::make_shared<State>(app, std::move(draft), completion_service, std::move(completion))) {
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
void FilesLocationDialog::publish_completion(FilesCompletionReply reply) { state_->publish_completion(std::move(reply)); }
} // namespace ck::vision
