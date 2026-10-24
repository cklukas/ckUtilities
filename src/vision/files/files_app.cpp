// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "files_location_dialog.hpp"
#include "ck/vision/keymap.hpp"
#include <cvision/core/text.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/text_view.hpp>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>
#include <stdexcept>
namespace ck::vision {
namespace {
// Keyboard activation belongs to the suite command/keymap layer. Keep the
// SDK's native double-click hit testing and activation callback, while passing
// Enter and F2 to command routing (F2 is reserved for Rename).
class FilesCellGrid final : public ckv::widgets::CellGrid {
  public:
    bool on_key(const ckv::KeyEvent &event) override {
        if (event.chord.key == ckv::Key::Enter || event.chord.key == ckv::Key::F2)
            return false;
        return CellGrid::on_key(event);
    }
};
class PaneSurface final : public ckv::ui::View {
  public:
    ckv::widgets::TextView *heading, *footer;
    ckv::widgets::CellGrid *grid;
    explicit PaneSurface(FilesPaneGridModel &model) {
        auto header = std::make_unique<ckv::widgets::TextView>();
        heading = header.get();
        auto cells = std::make_unique<FilesCellGrid>();
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
    if (reply.state == FilesPaneReplyState::Cancelled &&
        (!reply.error ||
         (reply.error->error.domain == ck::files::ErrorDomain::Adapter &&
          reply.error->error.code == static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead))))
        return "Read cancelled";
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
std::string preview_text(const FilesPanePreview &preview) {
    static constexpr char digits[] = "0123456789abcdef";
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t first = 0; first < preview.bytes.size(); first += 16) {
        out << std::setw(8) << first << "  ";
        const auto count = std::min<std::size_t>(16, preview.bytes.size() - first);
        for (std::size_t i = 0; i < 16; ++i) {
            if (i < count) {
                const auto byte = std::to_integer<unsigned char>(preview.bytes[first + i]);
                out << digits[byte >> 4] << digits[byte & 0x0f] << ' ';
            } else
                out << "   ";
            if (i == 7)
                out << ' ';
        }
        out << " | ";
        for (std::size_t i = 0; i < count; ++i) {
            const auto byte = std::to_integer<unsigned char>(preview.bytes[first + i]);
            out << ((byte >= 0x20 && byte <= 0x7e) ? static_cast<char>(byte) : '.');
        }
        out << '\n';
    }
    if (!preview.complete)
        out << "[Preview limited to the configured byte bound.]\n";
    return out.str();
}
} // namespace
struct FilesApp::State {
    struct Pane {
        FilesPaneGridModel model;
        FilesPaneTicket navigation, request;
        ck::files::PaneStateStamp stamp;
        std::shared_ptr<const FilesPaneLocation> active, requested;
        std::optional<FilesPaneViewport> cache, blocked;
        FilesPaneViewport inflight;
        bool navigating = false, reading = false, navigation_error = false, entering = false, homing = false;
        bool change_error = false, cancelling = false, preparing_cursor = false;
        std::optional<FilesPaneReplyKind> changing;
        std::optional<ck::files::PaneView> queued_view;
        ck::files::PaneView view;
        std::vector<std::shared_ptr<const FilesPaneLocation>> history;
        std::size_t history_position = 0;
        std::optional<std::size_t> history_target;
        std::vector<std::shared_ptr<const FilesPaneLocation>> bookmarks;
        std::optional<FilesPaneTicket> preview_request;
        bool previewing = false;
        ck::files::NameEncoding encoding = ck::files::NameEncoding::PosixBytes;
        ck::files::IndexedPaneSelection selection;
        std::optional<std::uint64_t> worker_focus;
        std::optional<ck::files::PaneRowId> range_start;
        std::string error;
    };
    ckv::ui::Application &application;
    std::array<FilesPaneService *, 2> services;
    std::array<FilesCompletionService *, 2> completion_services;
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
    std::optional<ckv::widgets::DescriptorDialogPresentation> view_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> properties_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> preview_dialog;
    std::unique_ptr<FilesLocationDialog> location_dialog;
    bool close_after_location = false;
    ckv::widgets::Window *view_dialog_window = nullptr;
    std::weak_ptr<void> view_dialog_lifetime;
    bool close_after_view = false;
    ckv::widgets::Window *properties_dialog_window = nullptr;
    std::weak_ptr<void> properties_dialog_lifetime;
    ckv::widgets::Window *preview_dialog_window = nullptr;
    std::weak_ptr<void> preview_dialog_lifetime;
    ckv::ui::CommandId switch_id = ckv::ui::kInvalidCommand, cancel_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId enter_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId history_back_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId history_forward_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bookmark_add_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bookmark_remove_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId refresh_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId properties_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_id = ckv::ui::kInvalidCommand;
    SuiteCommandScope commands;
    State(ckv::ui::Application &app, std::array<FilesPaneService *, 2> supplied, FilesAppWake &signal,
          std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> quit, ThemeScheme theme,
          std::array<FilesCompletionService *, 2> supplied_completion)
        : application(app), services(supplied), completion_services(supplied_completion), wake(signal),
          host_quit(std::move(quit)), commands(app.commands()) {
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
            application.commands().set_command_scope(switch_id, {.contexts = {"ck.files.pane"}});
            application.commands().set_command_scope(cancel_id, {.contexts = {"ck.files.pane"}});
            application.commands().set_enabled_predicate(switch_id, [this] { return !closing && !modal_pending(); });
            application.commands().set_enabled_predicate(
                cancel_id, [this] { return !closing && !modal_pending() && pending(panes[active]); });
            const auto own = [&](std::string_view key, std::function<void()> handler, std::function<bool()> enabled) {
                const auto id =
                    commands.own(declare_suite_command(application.commands(), "ck-files", key, std::move(handler)));
                if (id == ckv::ui::kInvalidCommand)
                    throw std::logic_error("missing Files command metadata");
                application.commands().set_enabled_predicate(id, std::move(enabled));
                application.commands().set_command_scope(id, {.contexts = {"ck.files.pane"}});
                return id;
            };
            const auto row_ready = [this] { return ready(active) && panes[active].model.target().has_value(); };
            const auto location_id =
                own("ck.files.location", [this] { show_location(); }, [this] { return can_edit_location(active); });
            const auto parent_id = own(
                "ck.files.parent", [this] { navigate_relative(active, false); },
                [this] { return relative_ready(active, false); });
            const auto root_id = own(
                "ck.files.root", [this] { navigate_relative(active, true); },
                [this] { return relative_ready(active, true); });
            const auto home_id =
                own("ck.files.home", [this] { navigate_home(active); }, [this] { return ready(active); });
            history_back_id = own(
                "ck.files.history_back", [this] { navigate_history_back(active); },
                [this] { return ready(active) && panes[active].history_position > 0; });
            history_forward_id = own(
                "ck.files.history_forward", [this] { navigate_history_forward(active); },
                [this] {
                    return ready(active) && panes[active].history_position + 1 < panes[active].history.size();
                });
            bookmark_add_id = own(
                "ck.files.bookmark_add", [this] { add_bookmark(active); },
                [this] { return ready(active) && panes[active].active; });
            bookmark_remove_id = own(
                "ck.files.bookmark_remove", [this] { remove_bookmark(active); },
                [this] {
                    return ready(active) && panes[active].active &&
                           std::any_of(panes[active].bookmarks.begin(), panes[active].bookmarks.end(),
                                       [this](const auto &bookmark) { return *bookmark == *panes[active].active; });
                });
            refresh_id = own("ck.files.refresh", [this] { refresh(active); }, [this] { return ready(active); });
            properties_id = own("ck.files.properties", [this] { show_properties(); }, [this] {
                return ready(active) && panes[active].model.cursor_row();
            });
            preview_id = own("ck.files.preview", [this] { show_preview(); }, [this] {
                return ready(active) && panes[active].model.cursor_kind() == ck::files::ObjectKind::RegularFile;
            });
            enter_id = own(
                "ck.files.enter_directory", [this] { enter_directory(active); },
                [this] {
                    return ready(active) && panes[active].model.target() &&
                           panes[active].model.cursor_kind() == ck::files::ObjectKind::Directory;
                });
            const auto visible_ready = [this] { return ready(active) && panes[active].model.row_count() != 0; };
            const auto toggle = own(
                "ck.files.toggle_mark",
                [this] {
                    auto target = panes[active].model.target();
                    auto marked = panes[active].model.cursor_marked();
                    if (target && marked)
                        change_marks(active, ck::files::IndexedMarkRow{target->row, !*marked});
                },
                row_ready);
            const auto mark_visible = own(
                "ck.files.mark_visible", [this] { change_marks(active, ck::files::IndexedMarkVisible{true}); },
                visible_ready);
            const auto unmark_visible = own(
                "ck.files.unmark_visible", [this] { change_marks(active, ck::files::IndexedMarkVisible{false}); },
                visible_ready);
            const auto invert = own(
                "ck.files.invert_marks", [this] { change_marks(active, ck::files::IndexedInvertVisible{}); },
                visible_ready);
            const auto clear = own(
                "ck.files.clear_marks", [this] { change_marks(active, ck::files::IndexedClearMarks{}); },
                [this] {
                    return ready(active) && panes[active].selection.marks && panes[active].selection.marks->total != 0;
                });
            const auto range = own(
                "ck.files.range_start",
                [this] {
                    if (const auto target = panes[active].model.target()) {
                        panes[active].range_start = target->row;
                        labels();
                    }
                },
                row_ready);
            const auto range_ready = [this] {
                return ready(active) && panes[active].range_start && panes[active].model.target();
            };
            const auto mark_range = own("ck.files.mark_range", [this] { mark_through(true); }, range_ready);
            const auto unmark_range = own("ck.files.unmark_range", [this] { mark_through(false); }, range_ready);
            const auto options_id =
                own("ck.files.view_options", [this] { show_view_options(); }, [this] { return ready(active); });
            const auto item = [](auto id, const char *title) {
                return ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{id, title});
            };
            SuiteShellOptions options;
            options.application_name = "Files";
            options.about_text = "Two independent read-only file locations.";
            options.theme = theme;
            options.application_menus = {
                ckv::widgets::MenuBarItem{
                    "&Location",
                    {item(location_id, "&Go to location..."), item(enter_id, "&Enter folder"),
                     item(parent_id, "&Parent folder"), item(root_id, "Location &root"),
                     item(home_id, "&Home location"), item(history_back_id, "Go &back"),
                     item(history_forward_id, "Go &forward"), item(bookmark_add_id, "Add &bookmark"),
                     item(bookmark_remove_id, "Remove bookmark"), item(refresh_id, "&Refresh listing"),
                     item(properties_id, "&Properties..."), item(preview_id, "Pre&view"),
                     ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{switch_id, "S&witch pane"}),
                     ckv::widgets::MenuItem::command(
                         ckv::widgets::CommandPresentation{cancel_id, "&Cancel activity"})}},
                ckv::widgets::MenuBarItem{
                    "&Marks",
                    {item(toggle, "&Toggle mark"), item(mark_visible, "&Mark visible"),
                     item(unmark_visible, "&Unmark visible"), item(invert, "&Invert visible marks"),
                     item(clear, "&Clear all marks (including hidden)"), item(range, "&Set range start"),
                     item(mark_range, "Mark th&rough cursor"), item(unmark_range, "Unmark through curs&or")}},
                ckv::widgets::MenuBarItem{"&View", {item(options_id, "View &options...")}}};
            options.application_status_items = {
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{enter_id, "&Enter folder"}, 35},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{parent_id, "&Parent folder"}, 32},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{switch_id, "&Switch pane"}, 30},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{toggle, "&Mark"}, 28},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{cancel_id, "&Cancel activity"}, 25},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{options_id, "&View options"}, 20}};
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
            for (std::size_t i = 0; i < 2; ++i) {
                panes[i].encoding = locations[i].encoding();
                navigate(i, std::move(locations[i]));
            }
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
        location_dialog.reset();
        view_dialog.reset();
        properties_dialog.reset();
        preview_dialog.reset();
        for (auto *surface : surfaces)
            if (surface) {
                surface->grid->clear_model();
                surface->grid->on_changed = {};
                surface->grid->on_activate = {};
                surface->grid->on_bounds_changed = {};
            }
        for (auto *service : services)
            if (service)
                service->request_stop();
        for (auto *service : completion_services)
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
        main->set_command_context("ck.files.pane");
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
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            auto *surface = surfaces[i];
            surface->grid->on_activate = [this, i] {
                if (active == i)
                    application.execute_command(enter_id);
            };
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
    bool modal_pending() const {
        return (confirmation && !confirmation->completed()) || (view_dialog && !view_dialog->completed()) ||
               (properties_dialog && !properties_dialog->completed()) ||
               (preview_dialog && !preview_dialog->completed()) ||
               (location_dialog && !location_dialog->completed());
    }
    bool can_edit_location(std::size_t i) const {
        return i < 2 && !closing && !modal_pending() && services[i]->running();
    }
    static bool pending(const Pane &p) { return p.navigating || p.reading || p.changing || p.queued_view || p.previewing; }
    bool ready(std::size_t i, bool continuation = false) const {
        return i < 2 && !closing && (continuation || !modal_pending()) && !pending(panes[i]) && panes[i].active &&
               panes[i].model.current() && services[i]->running();
    }
    std::string status(std::size_t i) const {
        const auto &p = panes.at(i);
        std::string text;
        if (p.cancelling)
            text = "Cancelling activity...";
        else if (p.navigating)
            text = p.entering ? "Opening folder..." : p.homing ? "Opening home location..." : "Opening location...";
        else if (p.preparing_cursor)
            text = "Reading current row for view change...";
        else if (p.previewing)
            text = "Reading preview...";
        else if (p.changing)
            text = *p.changing == FilesPaneReplyKind::Focus   ? "Preparing current row for view change..."
                   : *p.changing == FilesPaneReplyKind::Marks ? "Updating marks..."
                                                              : "Updating view...";
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
        if (p.selection.marks && p.selection.marks->total) {
            text += " | Marked: " + std::to_string(p.selection.marks->total);
            if (p.selection.marks->hidden)
                text += " (" + std::to_string(p.selection.marks->hidden) + " hidden; review required)";
        }
        if (p.range_start)
            text += " | Range start set";
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
        if (i >= 2 || closing || !location.is_valid())
            return false;
        return navigate(i, FilesPaneLocation(std::move(location)), {});
    }
    bool relative_ready(std::size_t i, bool root) const {
        if (!ready(i) || !panes[i].active)
            return false;
        const auto next = root ? panes[i].active->namespace_root() : panes[i].active->parent();
        return next.value() && *next.value() != *panes[i].active;
    }
    bool navigate_relative(std::size_t i, bool root) {
        if (!relative_ready(i, root))
            return false;
        auto next = root ? panes[i].active->namespace_root() : panes[i].active->parent();
        auto view = panes[i].view;
        view.display_substring.clear();
        return navigate(i, std::move(next).take_value(), std::move(view));
    }
    bool navigate(std::size_t i, FilesPaneLocation location, ck::files::PaneView view) {
        auto requested = std::make_shared<const FilesPaneLocation>(std::move(location));
        auto &p = panes[i];
        p.history_target.reset();
        auto range = p.model.viewport();
        auto submitted = services[i]->navigate(*requested, std::move(view), range.rows);
        return accept_navigation(i, submitted, std::move(requested));
    }
    bool navigate_home(std::size_t i) {
        if (!ready(i))
            return false;
        auto view = panes[i].view;
        view.display_substring.clear();
        panes[i].history_target.reset();
        return accept_navigation(i, services[i]->navigate_home(std::move(view), panes[i].model.viewport().rows), {},
                                 true);
    }
    bool refresh(std::size_t i) {
        if (!ready(i) || !panes[i].active)
            return false;
        return navigate(i, FilesPaneLocation(*panes[i].active), panes[i].view);
    }
    bool navigate_history(std::size_t i, std::size_t target) {
        if (!ready(i) || target >= panes[i].history.size() || target == panes[i].history_position)
            return false;
        auto view = panes[i].view;
        view.display_substring.clear();
        const bool accepted = navigate(i, FilesPaneLocation(*panes[i].history[target]), std::move(view));
        if (accepted)
            panes[i].history_target = target;
        return accepted;
    }
    bool navigate_history_back(std::size_t i) {
        if (i >= 2 || panes[i].history_position == 0)
            return false;
        return navigate_history(i, panes[i].history_position - 1);
    }
    bool navigate_history_forward(std::size_t i) {
        if (i >= 2 || panes[i].history_position + 1 >= panes[i].history.size())
            return false;
        return navigate_history(i, panes[i].history_position + 1);
    }
    bool add_bookmark(std::size_t i) {
        if (!ready(i) || !panes[i].active)
            return false;
        if (std::any_of(panes[i].bookmarks.begin(), panes[i].bookmarks.end(),
                        [&](const auto &bookmark) { return *bookmark == *panes[i].active; }))
            return false;
        panes[i].bookmarks.push_back(panes[i].active);
        labels();
        return true;
    }
    bool remove_bookmark(std::size_t i) {
        if (!ready(i) || !panes[i].active)
            return false;
        const auto before = panes[i].bookmarks.size();
        panes[i].bookmarks.erase(std::remove_if(panes[i].bookmarks.begin(), panes[i].bookmarks.end(),
                                                [&](const auto &bookmark) { return *bookmark == *panes[i].active; }),
                                 panes[i].bookmarks.end());
        labels();
        return panes[i].bookmarks.size() != before;
    }
    bool navigate_bookmark(std::size_t i, std::size_t bookmark) {
        if (!ready(i) || bookmark >= panes[i].bookmarks.size())
            return false;
        auto view = panes[i].view;
        view.display_substring.clear();
        return navigate(i, FilesPaneLocation(*panes[i].bookmarks[bookmark]), std::move(view));
    }
    bool accept_navigation(std::size_t i, FilesPaneSubmitted submitted,
                           std::shared_ptr<const FilesPaneLocation> requested, bool home = false) {
        auto &p = panes[i];
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Navigation request rejected";
            labels();
            return false;
        }
        p.navigation = p.request = submitted.ticket;
        p.requested = std::move(requested);
        p.navigating = true;
        p.entering = false;
        p.homing = home;
        p.reading = false;
        p.changing.reset();
        p.queued_view.reset();
        p.model.set_input_locked(false);
        p.cancelling = p.change_error = false;
        p.preparing_cursor = false;
        p.error.clear();
        p.navigation_error = false;
        p.blocked.reset();
        watch();
        labels();
        return true;
    }
    bool enter_directory(std::size_t i) {
        if (!ready(i) || panes[i].model.cursor_kind() != ck::files::ObjectKind::Directory)
            return false;
        auto &p = panes[i];
        const auto target = p.model.target();
        if (!target)
            return false;
        auto view = p.view;
        view.display_substring.clear();
        p.history_target.reset();
        const auto range = p.model.viewport();
        const auto submitted =
            services[i]->enter_directory(p.navigation, target->stamp, target->row, std::move(view), range.rows);
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Folder request rejected";
            labels();
            return false;
        }
        p.navigation = p.request = submitted.ticket;
        p.requested.reset(); // Only the worker may bind the requested native route.
        p.navigating = p.entering = true;
        p.homing = false;
        p.reading = false;
        p.cancelling = p.change_error = p.navigation_error = p.preparing_cursor = false;
        p.error.clear();
        p.blocked.reset();
        p.inflight = {0, range.rows};
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool begin_change(std::size_t i, FilesPaneChange command, bool continuation = false) {
        if (!ready(i, continuation))
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto kind = std::holds_alternative<FilesPaneFocusRow>(command)      ? FilesPaneReplyKind::Focus
                          : std::holds_alternative<FilesPaneChangeMarks>(command) ? FilesPaneReplyKind::Marks
                                                                                  : FilesPaneReplyKind::View;
        auto submitted = services[i]->change(p.navigation, p.stamp, std::move(command), {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Pane change request rejected";
            p.change_error = true;
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.inflight = range;
        p.changing = kind;
        p.cancelling = p.change_error = false;
        p.error.clear();
        p.navigation_error = false;
        p.blocked.reset();
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool change_marks(std::size_t i, ck::files::IndexedMarkCommand command) {
        return begin_change(i, FilesPaneChangeMarks{std::move(command)});
    }
    void mark_through(bool marked) {
        auto &p = panes[active];
        if (p.range_start)
            if (const auto target = p.model.target())
                change_marks(active, ck::files::IndexedMarkRange{*p.range_start, target->row, marked});
    }
    bool change_view(std::size_t i, ck::files::PaneView view) {
        if (!ready(i) || !ck::files::valid_pane_order({view.sort, view.descending, view.folders_first}) ||
            view.display_substring.size() > 4096)
            return false;
        auto &p = panes[i];
        if (const auto target = p.model.target(); target && p.worker_focus != p.model.cursor().row) {
            if (!begin_change(i, FilesPaneFocusRow{target->row}))
                return false;
            p.queued_view = std::move(view);
            return true;
        }
        // Wheel scrolling can leave the cursor outside the displayed cache.
        // Read its exact dataset position before asking the worker to validate
        // a row identity; never manufacture one from the position.
        if (p.model.row_count() && !p.model.target() && p.worker_focus != p.model.cursor().row) {
            const auto range = p.model.viewport();
            const auto first = p.model.cursor().row;
            auto submitted = services[i]->read_page(p.navigation, p.stamp, {first, range.rows});
            if (submitted.state != FilesPaneSubmission::Accepted)
                return false;
            p.request = submitted.ticket;
            p.inflight = {first, range.rows};
            p.reading = p.preparing_cursor = true;
            p.queued_view = std::move(view);
            p.cancelling = p.change_error = false;
            p.error.clear();
            p.blocked.reset();
            p.model.set_input_locked(true);
            watch();
            labels();
            return true;
        }
        return begin_change(i, FilesPaneChangeView{std::move(view)});
    }
    void show_location() {
        if (!can_edit_location(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto service_limits = services[pane]->limits();
        FilesLocationTextLimits limits;
        limits.path.max_native_bytes = service_limits.path_bytes;
        limits.components = service_limits.location_components;
        limits.text_bytes =
            std::max(limits.text_bytes, service_limits.path_bytes * 8 + service_limits.location_components * 4 + 8);
        const auto seed = panes[pane].navigating && panes[pane].requested ? panes[pane].requested
                          : panes[pane].active                            ? panes[pane].active
                                                                          : panes[pane].requested;
        auto draft = seed ? FilesLocationDraft::start(*seed, limits)
                          : FilesLocationDraft::new_location(panes[pane].encoding, limits);
        // An initial address rejected by the service budget is not a usable
        // seed. Recovery still offers empty input, never a guessed location.
        if (!draft.value() && !panes[pane].active)
            draft = FilesLocationDraft::new_location(panes[pane].encoding, limits);
        if (!draft.value()) {
            panes[pane].error = "This location cannot be edited within the configured limits";
            labels();
            return;
        }
        location_dialog = std::make_unique<FilesLocationDialog>(
            application, shell->desktop(), shell->roles(), std::move(draft).take_value(), completion_services[pane],
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation](std::optional<FilesPaneLocation> location) {
                if (weak.expired())
                    return;
                if (close_after_location) {
                    close_after_location = false;
                    request_close();
                    return;
                }
                if (!location)
                    return;
                if (panes[pane].navigation != navigation) {
                    panes[pane].error = "Location changed while its editor was open";
                    labels();
                    return;
                }
                if (!can_edit_location(pane)) {
                    panes[pane].error = "The location service is unavailable";
                    labels();
                    return;
                }
                if (!panes[pane].navigating && panes[pane].active && panes[pane].model.current() &&
                    *panes[pane].active == *location)
                    return;
                auto view = panes[pane].view;
                view.display_substring.clear();
                navigate(pane, std::move(*location), std::move(view));
            });
    }
    void show_view_options() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto &view = panes[pane].view;
        using namespace ckv::widgets;
        DialogDescriptor descriptor;
        descriptor.title = "View options";
        descriptor.resizable = true;
        descriptor.fields = {
            FieldDescriptor{.label = "&Sort by",
                            .kind = FieldKind::Combo,
                            .options = {"Name", "Type", "Bytes", "Modified (UTC)"},
                            .initial_selection = static_cast<int>(view.sort)},
            FieldDescriptor{.label = "&Descending", .kind = FieldKind::Check, .initial_checked = view.descending},
            FieldDescriptor{.label = "&Folders first", .kind = FieldKind::Check, .initial_checked = view.folders_first},
            FieldDescriptor{
                .label = "Show &hidden items", .kind = FieldKind::Check, .initial_checked = view.show_hidden},
            FieldDescriptor{.label = "Displayed name &contains",
                            .initial_text = view.display_substring,
                            .validate = [](const std::string &text) { return text.size() <= 4096; },
                            .description = "Literal substring, at most 4096 UTF-8 bytes; no wildcards."},
            FieldDescriptor{
                .label = "Match &ASCII case", .kind = FieldKind::Check, .initial_checked = view.ascii_case_sensitive},
            FieldDescriptor{
                .label = "Filters match escaped display names. Case folding is ASCII-only. Hidden marks stay marked.",
                .kind = FieldKind::Note}};
        descriptor.buttons = {{"&Apply", ButtonRole::Accept, {}}, {"&Cancel", ButtonRole::Dismiss, {}}};
        view_dialog.emplace(present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        view_dialog_window = shell->desktop().active_window();
        view_dialog_lifetime = view_dialog_window->lifetime_token();
        view_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation](const DialogResult &result) {
                if (weak.expired())
                    return;
                view_dialog_window = nullptr;
                if (close_after_view) {
                    close_after_view = false;
                    request_close();
                    return;
                }
                if (!result.accepted)
                    return;
                if (panes[pane].navigation != navigation || !ready(pane)) {
                    panes[pane].error = "Location changed while view options were open";
                    labels();
                    return;
                }
                if (result.values.size() != 7 || result.checked.size() != 7 || result.selected.size() != 7 ||
                    result.selected[0] < 0 || result.selected[0] > 3)
                    throw std::logic_error("invalid Files view dialog result");
                ck::files::PaneView next;
                next.sort = static_cast<ck::files::PaneSortKey>(result.selected[0]);
                next.descending = result.checked[1];
                next.folders_first = result.checked[2];
                next.show_hidden = result.checked[3];
                next.display_substring = result.values[4];
                next.ascii_case_sensitive = result.checked[5];
                if (!change_view(pane, std::move(next))) {
                    panes[pane].error = "View change request rejected";
                    labels();
                }
            });
    }
    void show_properties() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto *row = panes[pane].model.cursor_row();
        if (!row)
            return;
        const auto display_name = row->display_name;
        const auto location = panes[pane].active ? panes[pane].active->display_text() : std::string("Unknown location");
        const auto kind = [&] {
            if (!row->metadata.kind.value())
                return std::string("Unknown");
            switch (*row->metadata.kind.value()) {
            case ck::files::ObjectKind::RegularFile: return std::string("File");
            case ck::files::ObjectKind::Directory: return std::string("Folder");
            case ck::files::ObjectKind::SymbolicLink: return std::string("Symbolic link");
            case ck::files::ObjectKind::Junction: return std::string("Junction");
            case ck::files::ObjectKind::ReparsePoint: return std::string("Reparse point");
            case ck::files::ObjectKind::Fifo: return std::string("FIFO");
            case ck::files::ObjectKind::Socket: return std::string("Socket");
            case ck::files::ObjectKind::CharacterDevice: return std::string("Character device");
            case ck::files::ObjectKind::BlockDevice: return std::string("Block device");
            case ck::files::ObjectKind::Other: return std::string("Other");
            }
            return std::string("Unknown");
        }();
        const auto number = [](const auto &value) {
            return value.value() ? std::to_string(*value.value()) : std::string("Unknown");
        };
        const auto timestamp = [](const auto &value) {
            if (!value.value())
                return std::string("Unknown");
            return std::to_string(value.value()->seconds()) + "." +
                   std::to_string(value.value()->nanoseconds()) + " UTC seconds";
        };
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Properties";
        descriptor.fields = {
            {.label = "Name: " + display_name, .kind = ckv::widgets::FieldKind::Note},
            {.label = "Location: " + location, .kind = ckv::widgets::FieldKind::Note},
            {.label = "Type: " + kind, .kind = ckv::widgets::FieldKind::Note},
            {.label = "Logical bytes: " + number(row->metadata.logical_size), .kind = ckv::widgets::FieldKind::Note},
            {.label = "Allocated bytes: " + number(row->metadata.allocated_size), .kind = ckv::widgets::FieldKind::Note},
            {.label = "Links: " + number(row->metadata.link_count), .kind = ckv::widgets::FieldKind::Note},
            {.label = "Modified: " + timestamp(row->metadata.modified), .kind = ckv::widgets::FieldKind::Note},
            {.label = "Observation is from the acknowledged listing; refresh to recheck native metadata.",
             .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Close", ckv::widgets::ButtonRole::Dismiss, {}}};
        properties_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        properties_dialog_window = shell->desktop().active_window();
        properties_dialog_lifetime = properties_dialog_window->lifetime_token();
        properties_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime)](const ckv::widgets::DialogResult &) {
            if (!weak.expired())
                properties_dialog_window = nullptr;
            });
    }
    void show_preview() {
        if (!ready(active) || panes[active].model.cursor_kind() != ck::files::ObjectKind::RegularFile)
            return;
        const auto pane = active;
        const auto target = panes[pane].model.target();
        if (!target)
            return;
        const auto submitted = services[pane]->preview(panes[pane].navigation, target->stamp, target->row);
        if (submitted.state != FilesPaneSubmission::Accepted) {
            panes[pane].error = "Preview request rejected";
            labels();
            return;
        }
        panes[pane].preview_request = submitted.ticket;
        panes[pane].previewing = true;
        labels();
    }
    void watch() {
        const bool pending = std::any_of(panes.begin(), panes.end(), [](const Pane &p) { return State::pending(p); });
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
    void consume_preview(std::size_t i, FilesPaneReply reply) {
        auto &p = panes[i];
        if (!p.preview_request || reply.request != *p.preview_request)
            return;
        p.preview_request.reset();
        p.previewing = false;
        p.cancelling = false;
        if (reply.state != FilesPaneReplyState::Ready || !reply.preview) {
            p.error = failure(reply);
            labels();
            return;
        }
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Preview";
        descriptor.resizable = true;
        descriptor.fields = {
            {.label = "Bounded hexadecimal preview; display only. No execution or write operation is available.",
             .kind = ckv::widgets::FieldKind::Note},
            {.label = "Bytes", .initial_text = preview_text(*reply.preview), .kind = ckv::widgets::FieldKind::Memo},
        };
        descriptor.buttons = {{"&Close", ckv::widgets::ButtonRole::Dismiss, {}}};
        preview_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        preview_dialog_window = shell->desktop().active_window();
        preview_dialog_lifetime = preview_dialog_window->lifetime_token();
        preview_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime)](const ckv::widgets::DialogResult &) {
                if (!weak.expired())
                    preview_dialog_window = nullptr;
            });
        labels();
    }
    void consume(std::size_t i, FilesPaneReply reply) {
        auto &p = panes[i];
        if (p.preview_request && reply.request == *p.preview_request) {
            consume_preview(i, std::move(reply));
            return;
        }
        if (reply.navigation != p.navigation || reply.request != p.request)
            return;
        const bool navigation = reply.request == reply.navigation;
        const bool reading_cursor = p.preparing_cursor;
        const bool preparing_view = p.changing == FilesPaneReplyKind::Focus || reading_cursor;
        auto queued_view = std::move(p.queued_view);
        p.queued_view.reset();
        const bool cancelled = p.cancelling;
        p.stamp = reply.stamp;
        p.navigating = p.entering = p.homing = false;
        p.reading = false;
        p.changing.reset();
        p.cancelling = false;
        p.preparing_cursor = false;
        p.model.set_input_locked(false);
        if (reply.kind == FilesPaneReplyKind::View && reply.change_outcome == FilesPaneChangeOutcome::Applied)
            p.range_start.reset();
        bool adopted = false;
        if (reply.page && reply.page->stamp().current()) {
            if (p.model.publish(reply.page, navigation || (reply.kind == FilesPaneReplyKind::View &&
                                                           reply.change_outcome == FilesPaneChangeOutcome::Applied))) {
                adopted = true;
                if (navigation) {
                    p.active = reply.active_location;
                    p.requested = reply.requested_location;
                    p.navigation_error = false;
                    p.range_start.reset();
                    if (p.active) {
                        if (p.history_target) {
                            p.history_position = *p.history_target;
                            p.history_target.reset();
                        } else if (p.history.empty() || *p.history.back() != *p.active) {
                            if (!p.history.empty() && p.history_position + 1 < p.history.size())
                                p.history.erase(p.history.begin() + static_cast<std::ptrdiff_t>(p.history_position + 1),
                                                p.history.end());
                            p.history.push_back(p.active);
                            p.history_position = p.history.size() - 1;
                        }
                    }
                }
                p.view = reply.page->view();
                p.selection = reply.page->selection();
                p.worker_focus = reply.page->focused_position();
                p.cache = FilesPaneViewport{reply.page->first(), reply.page->rows().size()};
                p.blocked.reset();
                surfaces[i]->grid->model_changed();
            }
        }
        if (reply.state == FilesPaneReplyState::Ready && adopted) {
            if (!p.navigation_error && !p.change_error)
                p.error.clear();
            if (cancelled && navigation) {
                p.error = "Location opened before cancellation completed";
                p.change_error = true;
            } else if (cancelled && preparing_view) {
                p.error = "View change cancelled before application";
                p.change_error = true;
            } else if (cancelled && reply.change_outcome == FilesPaneChangeOutcome::Applied) {
                p.error = "Change applied before cancellation completed";
                p.change_error = true;
            }
            if (queued_view && reading_cursor && !cancelled) {
                if (const auto target = p.model.target()) {
                    if (begin_change(i, FilesPaneFocusRow{target->row}, true))
                        p.queued_view = std::move(queued_view);
                } else {
                    p.error = "View not applied; current row was not delivered";
                    p.change_error = true;
                }
            } else if (queued_view && reply.kind == FilesPaneReplyKind::Focus && !cancelled &&
                       reply.change_outcome == FilesPaneChangeOutcome::Applied)
                begin_change(i, FilesPaneChangeView{std::move(*queued_view)}, true);
            return;
        }
        p.error = reply.state == FilesPaneReplyState::Ready ? "Stale page rejected" : failure(reply);
        if (reply.change_outcome == FilesPaneChangeOutcome::Applied)
            p.error = (preparing_view ? "View not applied; current row acknowledged; " : "Change applied; ") + p.error;
        else if (reply.change_outcome == FilesPaneChangeOutcome::Unknown)
            p.error = "Change outcome unknown; reopen location before further actions. " + p.error;
        else if (reply.change_outcome == FilesPaneChangeOutcome::NotApplied)
            p.error = "Change not applied; " + p.error;
        else if (preparing_view)
            p.error = "View not applied; " + p.error;
        p.change_error = preparing_view || reply.change_outcome != FilesPaneChangeOutcome::NotRequested;
        p.navigation_error = navigation;
        if (navigation && reply.active_location)
            p.active = reply.active_location;
        if (!adopted)
            p.blocked = p.inflight;
    }
    void page(std::size_t i) {
        auto &p = panes[i];
        if (p.navigating || p.changing || p.queued_view || p.previewing || !p.active || !surfaces[i]->visible() || !p.stamp.current() ||
            !services[i]->running())
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
                if (auto completion = completion_services[i] ? completion_services[i]->take() : std::nullopt)
                    if (location_dialog)
                        location_dialog->publish_completion(std::move(*completion));
                if (!services[i]->running() && pending(panes[i])) {
                    const bool changing = panes[i].changing.has_value();
                    panes[i].navigating = panes[i].reading = false;
                    panes[i].previewing = false;
                    panes[i].preview_request.reset();
                    panes[i].changing.reset();
                    panes[i].queued_view.reset();
                    panes[i].model.set_input_locked(false);
                    panes[i].error = changing ? "Worker stopped; change outcome unknown" : "Worker stopped";
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
        if (closing || modal_pending())
            return;
        active = 1 - active;
        layout();
        application.set_focus(surfaces[active]->grid);
        wake.request();
    }
    void cancel_read() {
        if (!closing && !modal_pending() && pending(panes[active])) {
            panes[active].queued_view.reset();
            const auto request = panes[active].preview_request.value_or(panes[active].request);
            services[active]->cancel(request);
            panes[active].cancelling = true;
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
        for (auto *service : completion_services)
            if (service)
                service->request_stop();
        host_quit(); // No join in dispatch; the composition owner joins afterwards.
    }
    void request_close() {
        if (closing)
            return;
        if (location_dialog && !location_dialog->completed()) {
            close_after_location = true;
            location_dialog->close();
            return;
        }
        if (view_dialog && !view_dialog->completed()) {
            close_after_view = true;
            if (view_dialog_window && !view_dialog_lifetime.expired())
                view_dialog_window->close();
            return;
        }
        if (properties_dialog && !properties_dialog->completed()) {
            if (properties_dialog_window && !properties_dialog_lifetime.expired())
                properties_dialog_window->close();
            return;
        }
        if (preview_dialog && !preview_dialog->completed()) {
            if (preview_dialog_window && !preview_dialog_lifetime.expired())
                preview_dialog_window->close();
            return;
        }
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
            .label = "File reads or pane updates are still running. Stopping may wait for blocked storage to respond.",
            .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {
            {"&Stay", ckv::widgets::ButtonRole::Accept, {}},
            {"Stop activity and e&xit", ckv::widgets::ButtonRole::Dismiss, [this] { exit_selected = true; }}};
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
                   ThemeScheme theme, std::array<FilesCompletionService *, 2> completion_services)
    : state_(std::make_unique<State>(application, services, wake, std::move(locations), std::move(host_quit), theme,
                                     completion_services)) {}
FilesApp::~FilesApp() = default;
bool FilesApp::navigate(std::size_t pane, ck::files::NativeAbsolutePath location) {
    return state_->navigate(pane, std::move(location));
}
bool FilesApp::enter_directory(std::size_t pane) { return state_->enter_directory(pane); }
bool FilesApp::navigate_parent(std::size_t pane) { return state_->navigate_relative(pane, false); }
bool FilesApp::navigate_root(std::size_t pane) { return state_->navigate_relative(pane, true); }
bool FilesApp::navigate_home(std::size_t pane) { return state_->navigate_home(pane); }
bool FilesApp::refresh(std::size_t pane) { return state_->refresh(pane); }
bool FilesApp::navigate_history_back(std::size_t pane) { return state_->navigate_history_back(pane); }
bool FilesApp::navigate_history_forward(std::size_t pane) { return state_->navigate_history_forward(pane); }
bool FilesApp::add_bookmark(std::size_t pane) { return state_->add_bookmark(pane); }
bool FilesApp::remove_bookmark(std::size_t pane) { return state_->remove_bookmark(pane); }
bool FilesApp::navigate_bookmark(std::size_t pane, std::size_t bookmark) {
    return state_->navigate_bookmark(pane, bookmark);
}
std::shared_ptr<const FilesPaneLocation> FilesApp::location(std::size_t pane) const {
    return state_->panes.at(pane).active;
}
bool FilesApp::change_marks(std::size_t pane, ck::files::IndexedMarkCommand command) {
    return state_->change_marks(pane, std::move(command));
}
bool FilesApp::change_view(std::size_t pane, ck::files::PaneView view) {
    return state_->change_view(pane, std::move(view));
}
const ck::files::PaneView &FilesApp::view(std::size_t pane) const { return state_->panes.at(pane).view; }
const ck::files::IndexedPaneSelection &FilesApp::selection(std::size_t pane) const {
    return state_->panes.at(pane).selection;
}
void FilesApp::switch_pane() { state_->switch_pane(); }
void FilesApp::cancel_read() { state_->cancel_read(); }
void FilesApp::request_close() { state_->request_close(); }
bool FilesApp::close_confirmation_pending() const { return state_->confirmation && !state_->confirmation->completed(); }
bool FilesApp::view_options_pending() const { return state_->view_dialog && !state_->view_dialog->completed(); }
bool FilesApp::properties_pending() const {
    return state_->properties_dialog && !state_->properties_dialog->completed();
}
bool FilesApp::preview_pending() const {
    return std::any_of(state_->panes.begin(), state_->panes.end(), [](const auto &pane) { return pane.previewing; }) ||
           (state_->preview_dialog && !state_->preview_dialog->completed());
}
bool FilesApp::location_editor_pending() const {
    return state_->location_dialog && !state_->location_dialog->completed();
}
std::size_t FilesApp::history_size(std::size_t pane) const { return state_->panes.at(pane).history.size(); }
std::size_t FilesApp::history_position(std::size_t pane) const {
    return state_->panes.at(pane).history_position;
}
std::size_t FilesApp::bookmark_count(std::size_t pane) const { return state_->panes.at(pane).bookmarks.size(); }
std::size_t FilesApp::active_pane() const noexcept { return state_->active; }
bool FilesApp::narrow() const noexcept { return state_->compact; }
bool FilesApp::pending(std::size_t pane) const {
    const auto &p = state_->panes.at(pane);
    return State::pending(p);
}
std::string FilesApp::status(std::size_t pane) const { return state_->status(pane); }
std::optional<FilesAppTarget> FilesApp::target(std::size_t pane) const {
    const auto &p = state_->panes.at(pane);
    if (state_->closing || p.navigating || p.changing || p.queued_view || p.previewing ||
        !state_->services.at(pane)->running())
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
