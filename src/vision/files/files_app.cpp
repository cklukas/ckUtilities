// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "ck/vision/keymap.hpp"
#include <cvision/core/text.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/text_view.hpp>
#include <algorithm>
#include <stdexcept>
namespace ck::vision {
namespace {
class PaneSurface final : public ckv::ui::View {
  public:
    ckv::widgets::TextView *heading, *footer;
    ckv::widgets::CellGrid *grid;
    explicit PaneSurface(FilesPaneGridModel &model) {
        auto header = std::make_unique<ckv::widgets::TextView>();
        heading = header.get();
        auto cells = std::make_unique<ckv::widgets::CellGrid>();
        grid = cells.get();
        grid->set_model(model);
        auto status = std::make_unique<ckv::widgets::TextView>();
        footer = status.get();
        for (auto text : {heading, footer}) {
            text->set_focus_policy(ckv::ui::FocusPolicy::None);
            text->set_vertical_scrollbar_policy(ckv::widgets::ScrollbarPolicy::Hidden);
            text->set_horizontal_scrollbar_policy(ckv::widgets::ScrollbarPolicy::Hidden);
        }
        add_child(std::move(header));
        add_child(std::move(cells));
        add_child(std::move(status));
    }
    void on_resized() override {
        const auto size = bounds();
        const int h = std::max(0, size.height), w = std::max(0, size.width);
        const int header = std::min(2, h), foot = std::min(2, h - header);
        heading->set_bounds({0, 0, w, header});
        grid->set_bounds({0, header, w, h - header - foot});
        footer->set_bounds({0, h - foot, w, foot});
    }
};
bool contains(const ckv::ui::View *root, const ckv::ui::View *view) {
    for (auto current = view; current; current = current->parent())
        if (current == root)
            return true;
    return false;
}
std::string failure(const FilesPaneReply &reply) {
    if (reply.error) {
        const auto &e = *reply.error;
        const char *domain = e.error.domain == ck::files::ErrorDomain::Posix      ? "POSIX"
                             : e.error.domain == ck::files::ErrorDomain::Win32    ? "Win32"
                             : e.error.domain == ck::files::ErrorDomain::NtStatus ? "NTSTATUS"
                                                                                  : "Adapter";
        const char *message = e.state == ck::files::ObservationState::Denied        ? "Access denied"
                              : e.state == ck::files::ObservationState::Unsupported ? "Read unsupported"
                              : e.state == ck::files::ObservationState::Unknown     ? "Read could not be determined"
                              : e.state == ck::files::ObservationState::Gone        ? "Location disappeared"
                              : e.state == ck::files::ObservationState::Changed     ? "Location changed while reading"
                                                                                    : "Read failed";
        return std::string(message) + ": " + domain + " " + std::to_string(e.error.code) + " " +
               ckv::text::sanitize_display_text(e.error.operation);
    }
    switch (reply.state) {
    case FilesPaneReplyState::Cancelled:
        return "Read cancelled";
    case FilesPaneReplyState::Stale:
        return "Listing changed; this page is stale";
    case FilesPaneReplyState::OutOfRange:
        return "Requested page is outside this listing";
    case FilesPaneReplyState::LimitExceeded:
        return "Configured read limit exceeded";
    case FilesPaneReplyState::WorkerFault:
        return reply.worker_fault == FilesPaneWorkerFault::Allocation ? "Worker stopped: insufficient memory"
                                                                      : "Worker stopped after an internal error";
    case FilesPaneReplyState::Failed:
        return "Read failed";
    case FilesPaneReplyState::Ready:
        return {};
    }
    return "Read failed";
}
} // namespace
struct FilesApp::State {
    struct Pane {
        FilesPaneGridModel model;
        FilesPaneTicket navigation, request;
        ck::files::PaneStateStamp stamp;
        std::shared_ptr<const ck::files::NativeAbsolutePath> active, requested;
        std::optional<FilesPaneViewport> cache, blocked;
        FilesPaneViewport inflight;
        bool navigating = false, reading = false, navigation_error = false;
        std::string error;
    };
    ckv::ui::Application &application;
    std::array<FilesPaneService *, 2> services;
    FilesAppWake &wake;
    std::function<void()> host_quit;
    std::array<Pane, 2> panes;
    std::unique_ptr<SuiteShell> shell;
    ckv::widgets::Window *window = nullptr;
    ckv::widgets::Splitter *splitter = nullptr;
    std::array<PaneSurface *, 2> surfaces{};
    std::size_t active = 0;
    bool compact = false, closing = false, bound = false, quit_owned = false, pumping = false, exit_selected = false;
    ckv::ui::Application::TimerId watchdog = 0;
    ckv::ui::Application::AttentionObserverId observer = 0;
    std::shared_ptr<void> lifetime = std::make_shared<int>(0);
    std::optional<ckv::widgets::DescriptorDialogPresentation> confirmation;
    ckv::ui::CommandId switch_id = ckv::ui::kInvalidCommand, cancel_id = ckv::ui::kInvalidCommand;
    SuiteCommandScope commands;
    State(ckv::ui::Application &app, std::array<FilesPaneService *, 2> supplied, FilesAppWake &signal,
          std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> quit, ThemeScheme theme)
        : application(app), services(supplied), wake(signal), host_quit(std::move(quit)), commands(app.commands()) {
        if (!services[0] || !services[1] || services[0] == services[1] || !host_quit)
            throw std::invalid_argument("invalid Files host");
        try {
            // Desktop installs and later clears its own fallback Quit handler
            // only when the host has none. Establish the lent host handler
            // before attaching the desktop so teardown cannot erase it.
            application.commands().set_handler(application.commands().standard().quit, host_quit);
            quit_owned = true;
            switch_id = commands.own(declare_suite_command(application.commands(), "ck-files", "ck.files.switch_pane",
                                                           [this] { switch_pane(); }));
            cancel_id = commands.own(declare_suite_command(application.commands(), "ck-files", "ck.files.cancel_read",
                                                           [this] { cancel_read(); }));
            if (switch_id == ckv::ui::kInvalidCommand || cancel_id == ckv::ui::kInvalidCommand)
                throw std::logic_error("missing Files command metadata");
            application.commands().set_enabled_predicate(
                switch_id, [this] { return !closing && !(confirmation && !confirmation->completed()); });
            application.commands().set_enabled_predicate(cancel_id, [this] {
                return !closing && !(confirmation && !confirmation->completed()) &&
                       (panes[active].navigating || panes[active].reading);
            });
            SuiteShellOptions options;
            options.application_name = "Files";
            options.about_text = "Two independent read-only file locations.";
            options.theme = theme;
            options.application_menus = {ckv::widgets::MenuBarItem{
                "&Location",
                {ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{switch_id, "S&witch pane"}),
                 ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{cancel_id, "&Cancel read"})}}};
            options.application_status_items = {
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{switch_id, "&Switch pane"}, 30},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{cancel_id, "&Cancel read"}, 25}};
            shell = std::make_unique<SuiteShell>(application, std::move(options));
            create_window();
            observer = application.add_attention_observer([this](auto kind) {
                if (closing)
                    return;
                if (kind == ckv::ui::Application::AttentionChange::Focus) {
                    for (std::size_t i = 0; i < 2; ++i)
                        if (surfaces[i] && contains(surfaces[i], application.focused()) && active != i) {
                            active = i;
                            layout();
                        }
                }
            });
            wake.bind([this] { pump(); });
            bound = true;
            application.commands().set_handler(application.commands().standard().quit, [this] { request_close(); });
            for (std::size_t i = 0; i < 2; ++i)
                navigate(i, std::move(locations[i]));
            application.set_focus(surfaces[0]->grid);
            pump();
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~State() { cleanup(); }
    void cleanup() noexcept {
        closing = true;
        lifetime.reset();
        if (bound) {
            wake.unbind();
            bound = false;
        }
        if (watchdog) {
            application.cancel_timer(watchdog);
            watchdog = 0;
        }
        if (observer) {
            application.remove_attention_observer(observer);
            observer = 0;
        }
        if (quit_owned) {
            application.commands().set_handler(application.commands().standard().quit, std::move(host_quit));
            quit_owned = false;
        }
        confirmation.reset();
        for (auto *surface : surfaces)
            if (surface) {
                surface->grid->clear_model();
                surface->grid->on_changed = {};
                surface->grid->on_bounds_changed = {};
            }
        for (auto *service : services)
            if (service)
                service->request_stop();
    }
    void create_window() {
        auto first = std::make_unique<PaneSurface>(panes[0].model),
             second = std::make_unique<PaneSurface>(panes[1].model);
        const std::array<PaneSurface *, 2> pane_views{first.get(), second.get()};
        auto split = std::make_unique<ckv::widgets::Splitter>(ckv::Rect{}, std::move(first), std::move(second));
        auto *split_view = split.get();
        auto main = std::make_unique<ckv::widgets::Window>("Files");
        auto *main_view = main.get();
        main->set_bounds(shell->desktop().content_area());
        main->set_min_size({40, 12});
        main->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
        main->set_content(std::move(split));
        shell->desktop().add_window(std::move(main));
        // Publish borrowed views only after ownership is established. If a
        // construction/allocation step above throws, cleanup has no dangling
        // local-view pointers to visit.
        surfaces = pane_views;
        splitter = split_view;
        window = main_view;
        for (auto *surface : surfaces) {
            surface->grid->on_changed = [this] { wake.request(); };
            surface->grid->on_bounds_changed = [this](ckv::Rect) {
                if (bound && !closing)
                    wake.request();
            };
        }
        splitter->on_bounds_changed = [this](ckv::Rect) {
            if (!closing)
                layout();
        };
        window->close_request = [this] {
            request_close();
            return false;
        };
        // The empty splitter starts with a zero anchored extent. Choose the
        // initial division once real window geometry has been established;
        // subsequent resizes preserve the user's divider position.
        splitter->set_split_position(std::max(0, splitter->bounds().width - 1) / 2);
        layout();
    }
    void layout() {
        if (!window || !splitter)
            return;
        compact = splitter->bounds().width < 100;
        for (std::size_t i = 0; i < 2; ++i)
            surfaces[i]->set_visible(!compact || i == active);
        labels();
        if (bound && !closing)
            wake.request();
    }
    std::string status(std::size_t i) const {
        const auto &p = panes.at(i);
        std::string text;
        if (p.navigating)
            text = "Opening location...";
        else if (!p.error.empty())
            text = p.error;
        else if (!p.active)
            text = "No completed location";
        else if (!p.model.current())
            text = "Stale listing";
        else if (p.reading)
            text = "Loading page...";
        else
            text = "Last scan: " + std::to_string(p.model.row_count()) + " items";
        if (p.active && !p.model.current() && text.find("Stale") == text.npos)
            text += "; stale listing";
        if (p.navigation_error && p.active)
            text += "; previous location retained";
        return text;
    }
    void labels() {
        if (!window)
            return;
        for (std::size_t i = 0; i < 2; ++i) {
            auto &p = panes[i];
            const std::string side = i == 0 ? "Left" : "Right";
            auto location = p.active ? p.active->display_text() : std::string("No completed location");
            auto title = (active == i ? "[Active] " : "[Other] ") + side + "\n" + location;
            if (surfaces[i]->heading->text() != title)
                surfaces[i]->heading->set_text(std::move(title));
            auto text = status(i);
            if (p.navigating && p.requested)
                text += "\n" + p.requested->display_text();
            if (surfaces[i]->footer->text() != text)
                surfaces[i]->footer->set_text(std::move(text));
        }
        const auto &other = panes[1 - active];
        window->set_footer(compact ? "F4 Switch pane | Other: " + std::string(active == 0 ? "Right " : "Left ") +
                                         (other.active ? other.active->display_text() : "Loading")
                                   : "Left and right locations are independent");
    }
    bool navigate(std::size_t i, ck::files::NativeAbsolutePath location) {
        if (i >= 2 || closing)
            return false;
        auto requested = std::make_shared<const ck::files::NativeAbsolutePath>(std::move(location));
        auto &p = panes[i];
        auto range = p.model.viewport();
        auto submitted = services[i]->navigate(*requested, {}, range.rows);
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Navigation request rejected";
            labels();
            return false;
        }
        p.navigation = p.request = submitted.ticket;
        p.requested = std::move(requested);
        p.navigating = true;
        p.reading = false;
        p.error.clear();
        p.navigation_error = false;
        p.blocked.reset();
        watch();
        labels();
        return true;
    }
    void watch() {
        const bool pending =
            std::any_of(panes.begin(), panes.end(), [](const Pane &p) { return p.navigating || p.reading; });
        if (pending && !watchdog)
            watchdog = application.start_timer(250'000'000, true, [this] { pump(); });
        if (!pending && watchdog) {
            application.cancel_timer(watchdog);
            watchdog = 0;
        }
    }
    bool covers(const Pane &p, FilesPaneViewport range) const {
        if (!p.cache || !p.model.current() || range.first < p.cache->first)
            return false;
        const auto count = std::min<std::uint64_t>(range.rows, p.model.row_count() - range.first);
        const auto offset = range.first - p.cache->first;
        return offset <= p.cache->rows && count <= p.cache->rows - offset;
    }
    void consume(std::size_t i, FilesPaneReply reply) {
        auto &p = panes[i];
        if (reply.navigation != p.navigation || reply.request != p.request)
            return;
        const bool navigation = reply.request == reply.navigation;
        p.stamp = reply.stamp;
        p.navigating = false;
        p.reading = false;
        if (reply.state == FilesPaneReplyState::Ready && reply.page && reply.page->stamp().current()) {
            if (p.model.publish(reply.page, navigation)) {
                if (navigation) {
                    p.active = reply.active_location;
                    p.requested = reply.requested_location;
                    p.navigation_error = false;
                    p.error.clear();
                } else if (!p.navigation_error)
                    p.error.clear();
                p.cache = FilesPaneViewport{reply.page->first(), reply.page->rows().size()};
                p.blocked.reset();
                surfaces[i]->grid->model_changed();
                return;
            }
        }
        p.error = reply.state == FilesPaneReplyState::Ready ? "Stale page rejected" : failure(reply);
        p.navigation_error = navigation;
        if (navigation && reply.active_location)
            p.active = reply.active_location;
        p.blocked = p.inflight;
    }
    void page(std::size_t i) {
        auto &p = panes[i];
        if (p.navigating || !p.active || !surfaces[i]->visible() || !p.stamp.current() || !services[i]->running())
            return;
        auto range = p.model.viewport();
        if (covers(p, range) || (p.blocked && *p.blocked == range) || (p.reading && p.inflight == range))
            return;
        auto submitted = services[i]->read_page(p.navigation, p.stamp, {range.first, range.rows});
        if (submitted.state == FilesPaneSubmission::Accepted) {
            p.request = submitted.ticket;
            p.inflight = range;
            p.reading = true;
            p.blocked.reset();
        } else if (submitted.state != FilesPaneSubmission::Busy) {
            p.blocked = range;
            p.error = "Page request rejected";
        }
    }
    void pump() {
        if (closing || pumping)
            return;
        pumping = true;
        try {
            for (std::size_t i = 0; i < 2; ++i) {
                if (auto reply = services[i]->take())
                    consume(i, std::move(*reply));
                if (!services[i]->running() && (panes[i].navigating || panes[i].reading)) {
                    panes[i].navigating = panes[i].reading = false;
                    panes[i].error = "Worker stopped";
                }
                page(i);
            }
            watch();
            labels();
            pumping = false;
        } catch (...) {
            pumping = false;
            throw;
        }
    }
    void switch_pane() {
        if (closing || (confirmation && !confirmation->completed()))
            return;
        active = 1 - active;
        layout();
        application.set_focus(surfaces[active]->grid);
        wake.request();
    }
    void cancel_read() {
        if (!closing && !(confirmation && !confirmation->completed()) &&
            services[active]->cancel(panes[active].request)) {
            panes[active].error = "Cancelling read...";
            labels();
        }
    }
    void finish_exit() {
        if (closing)
            return;
        closing = true;
        if (bound) {
            wake.unbind();
            bound = false;
        }
        if (watchdog) {
            application.cancel_timer(watchdog);
            watchdog = 0;
        }
        for (auto *service : services)
            service->request_stop();
        host_quit(); // No join in dispatch; the composition owner joins afterwards.
    }
    void request_close() {
        if (closing)
            return;
        if (confirmation && !confirmation->completed())
            return;
        if (!services[0]->working() && !services[1]->working()) {
            finish_exit();
            return;
        }
        exit_selected = false;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Close Files";
        descriptor.fields = {ckv::widgets::FieldDescriptor{
            .label = "Directory reads are still running. Stopping them may wait for blocked storage to respond.",
            .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {
            {"&Stay", ckv::widgets::ButtonRole::Accept, {}},
            {"Stop reads and e&xit", ckv::widgets::ButtonRole::Dismiss, [this] { exit_selected = true; }}};
        confirmation.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        confirmation->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime)](const ckv::widgets::DialogResult &) {
                if (!weak.expired() && exit_selected)
                    finish_exit();
            });
    }
};
FilesApp::FilesApp(ckv::ui::Application &application, std::array<FilesPaneService *, 2> services, FilesAppWake &wake,
                   std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> host_quit,
                   ThemeScheme theme)
    : state_(std::make_unique<State>(application, services, wake, std::move(locations), std::move(host_quit), theme)) {}
FilesApp::~FilesApp() = default;
bool FilesApp::navigate(std::size_t pane, ck::files::NativeAbsolutePath location) {
    return state_->navigate(pane, std::move(location));
}
void FilesApp::switch_pane() { state_->switch_pane(); }
void FilesApp::cancel_read() { state_->cancel_read(); }
void FilesApp::request_close() { state_->request_close(); }
bool FilesApp::close_confirmation_pending() const { return state_->confirmation && !state_->confirmation->completed(); }
std::size_t FilesApp::active_pane() const noexcept { return state_->active; }
bool FilesApp::narrow() const noexcept { return state_->compact; }
bool FilesApp::pending(std::size_t pane) const {
    const auto &p = state_->panes.at(pane);
    return p.navigating || p.reading;
}
std::string FilesApp::status(std::size_t pane) const { return state_->status(pane); }
std::optional<FilesAppTarget> FilesApp::target(std::size_t pane) const {
    const auto &p = state_->panes.at(pane);
    if (state_->closing || p.navigating || !state_->services.at(pane)->running())
        return {};
    if (auto target = p.model.target())
        return FilesAppTarget{p.navigation, std::move(*target)};
    return {};
}
const FilesPaneGridModel &FilesApp::model(std::size_t pane) const { return state_->panes.at(pane).model; }
ckv::widgets::CellGrid *FilesApp::grid(std::size_t pane) const { return state_->surfaces.at(pane)->grid; }
ckv::ui::CommandId FilesApp::switch_command() const noexcept { return state_->switch_id; }
ckv::ui::CommandId FilesApp::cancel_command() const noexcept { return state_->cancel_id; }
} // namespace ck::vision
