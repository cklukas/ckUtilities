// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "files_location_dialog.hpp"
#include "files_volume_source.hpp"
#include "ck/vision/keymap.hpp"
#include "name_mapping.hpp"
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
                              : e.state == ck::files::ObservationState::Unsupported
                                    ? (reply.kind == FilesPaneReplyKind::Mutation ? "Operation unsupported"
                                                                                   : "Read unsupported")
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

const char *archive_recovery_action(ck::files::OperationRecoveryAction action) noexcept {
    using Action = ck::files::OperationRecoveryAction;
    switch (action) {
    case Action::IgnoreTerminal: return "terminal";
    case Action::InspectUnpublishedStaging: return "inspect unpublished staging";
    case Action::VerifyDestination: return "verify destination";
    case Action::VerifyDestinationBeforeSourceRetirement: return "verify destination before source retirement";
    case Action::InspectAmbiguousPublication: return "inspect ambiguous publication";
    case Action::InspectAmbiguousSourceRetirement: return "inspect ambiguous source retirement";
    case Action::InspectAmbiguousDeletion: return "inspect ambiguous deletion";
    case Action::RequireReview: return "manual review required";
    }
    return "unknown recovery action";
}

const char *archive_recovery_state(ck::files::OperationJournalState state) noexcept {
    using State = ck::files::OperationJournalState;
    switch (state) {
    case State::Planned: return "planned";
    case State::Prepared: return "prepared";
    case State::Copied: return "copied";
    case State::Verified: return "verified";
    case State::CommitIntended: return "commit intended";
    case State::DestinationCommitted: return "destination committed";
    case State::SourceRemovalIntended: return "source removal intended";
    case State::SourceRetired: return "source retired";
    case State::Complete: return "complete";
    case State::Failed: return "failed";
    case State::Cancelled: return "cancelled";
    case State::RetainedSource: return "source retained";
    case State::NeedsReconciliation: return "needs reconciliation";
    }
    return "unknown state";
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
std::optional<ck::files::NativeName> parse_directory_component(std::string text,
                                                               ck::files::NameEncoding encoding) {
    auto posix = ck::files::NativeName::from_posix(std::move(text));
    if (!posix)
        return std::nullopt;
    if (encoding == ck::files::NameEncoding::PosixBytes)
        return std::move(*posix);
    if (encoding != ck::files::NameEncoding::WindowsUtf16)
        return std::nullopt;
    auto mapped = ck::files::map_native_name(*posix, encoding, {});
    if (!mapped.value())
        return std::nullopt;
    return std::move(mapped).take_value();
}
std::string volume_bytes(const std::optional<std::uint64_t> &value) {
    if (!value)
        return "Unknown";
    static constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double scaled = static_cast<double>(*value);
    std::size_t unit = 0;
    while (scaled >= 1024.0 && unit + 1 < std::size(units)) {
        scaled /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0)
        out << *value;
    else
        out << std::fixed << std::setprecision(scaled >= 10.0 ? 1 : 2) << scaled;
    out << ' ' << units[unit];
    return out.str();
}
std::string volume_failure(const FilesVolumeReply &reply) {
    if (reply.error) {
        const auto &error = *reply.error;
        const char *domain = error.error.domain == ck::files::ErrorDomain::Posix ? "POSIX"
                             : error.error.domain == ck::files::ErrorDomain::Win32 ? "Win32"
                                                                                     : "Adapter";
        return std::string(error.state == ck::files::ObservationState::Denied ? "Access denied"
                              : error.state == ck::files::ObservationState::Unsupported ? "Read unsupported"
                              : error.state == ck::files::ObservationState::Unknown ? "Read could not be determined"
                                                                                    : "Read failed") +
               ": " + domain + " " + std::to_string(error.error.code) + " " +
               ckv::text::sanitize_display_text(error.error.operation);
    }
    return reply.state == FilesVolumeReplyState::Cancelled ? "Volume scan cancelled" : "Volume scan failed";
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
        bool change_error = false, cancelling = false, preparing_cursor = false, mutating = false, renaming = false,
             deleting = false, trashing = false;
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
        bool volume_view = false;
    };
    ckv::ui::Application &application;
    std::array<FilesPaneService *, 2> services;
    std::array<FilesCompletionService *, 2> completion_services;
    FilesOperationPlanningService *planning_service = nullptr;
    FilesOperationExecutionService *execution_service = nullptr;
    FilesVolumeService *volume_service = nullptr;
    FilesVolumeArchivePlanningService *volume_archive_planning_service = nullptr;
    FilesVolumeArchiveService *volume_archive_service = nullptr;
    FilesVolumeArchiveRecoveryService *volume_archive_recovery_service = nullptr;
    std::shared_ptr<const ck::files::NativeAbsolutePath> operation_journal_root;
    FilesSettingsBinding settings;
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
    std::optional<ckv::widgets::DescriptorDialogPresentation> create_directory_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> rename_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> trash_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> delete_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> archive_recovery_dialog;
    std::unique_ptr<FilesOperationReviewDialog> operation_review_dialog;
    std::optional<FilesOperationPlanningTicket> operation_planning_request;
    std::optional<FilesOperationExecutionTicket> operation_execution_request;
    std::optional<FilesVolumeArchivePlanningTicket> volume_archive_planning_request;
    std::optional<FilesVolumeArchiveTicket> volume_archive_execution_request;
    bool volume_archive_recovery_request = false;
    std::optional<FilesVolumeArchiveRecoveryScan> volume_archive_recovery_scan;
    std::string volume_archive_recovery_error;
    std::unique_ptr<FilesVolumeArchiveReviewDialog> volume_archive_review_dialog;
    FilesVolumeRecord volume_archive_source;
    std::shared_ptr<const FilesPaneLocation> volume_archive_destination;
    std::optional<ck::files::FileOperation> operation_execution_kind;
    std::optional<ck::files::NameEncoding> operation_planning_encoding;
    FilesApp::OperationRevalidation operation_planning_revalidation;
    FilesOperationReviewDialog::Completion operation_planning_completion;
    std::unique_ptr<FilesLocationDialog> location_dialog;
    bool close_after_location = false;
    ckv::widgets::Window *view_dialog_window = nullptr;
    std::weak_ptr<void> view_dialog_lifetime;
    bool close_after_view = false;
    ckv::widgets::Window *properties_dialog_window = nullptr;
    std::weak_ptr<void> properties_dialog_lifetime;
    ckv::widgets::Window *preview_dialog_window = nullptr;
    std::weak_ptr<void> preview_dialog_lifetime;
    ckv::widgets::Window *create_directory_window = nullptr;
    std::weak_ptr<void> create_directory_lifetime;
    ckv::widgets::Window *rename_window = nullptr;
    std::weak_ptr<void> rename_lifetime;
    ckv::widgets::Window *trash_window = nullptr;
    std::weak_ptr<void> trash_lifetime;
    ckv::widgets::Window *delete_window = nullptr;
    std::weak_ptr<void> delete_lifetime;
    ckv::widgets::Window *archive_recovery_window = nullptr;
    std::weak_ptr<void> archive_recovery_lifetime;
    bool volume_request = false;
    std::size_t volume_pane = 0;
    ckv::ui::CommandId switch_id = ckv::ui::kInvalidCommand, cancel_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId enter_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId create_directory_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId rename_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId trash_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId delete_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId copy_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId move_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_recovery_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId history_back_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId history_forward_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bookmark_add_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bookmark_remove_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId refresh_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId properties_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_id = ckv::ui::kInvalidCommand;
    SuiteCommandScope commands;
    std::uint64_t operation_serial = 1;
    State(ckv::ui::Application &app, std::array<FilesPaneService *, 2> supplied, FilesAppWake &signal,
          std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> quit, ThemeScheme theme,
          std::array<FilesCompletionService *, 2> supplied_completion, FilesOperationPlanningService *planning,
          FilesOperationExecutionService *execution,
          std::shared_ptr<const ck::files::NativeAbsolutePath> journal_root, FilesSettingsBinding supplied_settings,
          FilesVolumeService *volumes, FilesVolumeArchivePlanningService *archive_planning,
          FilesVolumeArchiveService *archive_service, FilesVolumeArchiveRecoveryService *archive_recovery)
        : application(app), services(supplied), completion_services(supplied_completion), planning_service(planning),
          execution_service(execution), volume_service(volumes),
          volume_archive_planning_service(archive_planning), volume_archive_service(archive_service),
          volume_archive_recovery_service(archive_recovery),
          operation_journal_root(std::move(journal_root)),
          settings(std::move(supplied_settings)), wake(signal),
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
            application.commands().bind_key({ckv::Key::Escape, ckv::Modifier::None, {}}, cancel_id);
            application.commands().set_enabled_predicate(switch_id,
                                                         [this] { return availability("ck.files.switch_pane").enabled; });
            application.commands().set_enabled_predicate(cancel_id,
                                                         [this] { return availability("ck.files.cancel_read").enabled; });
            const auto own = [&](std::string_view key, std::function<void()> handler, std::function<bool()> enabled) {
                const auto id =
                    commands.own(declare_suite_command(application.commands(), "ck-files", key, std::move(handler)));
                if (id == ckv::ui::kInvalidCommand)
                    throw std::logic_error("missing Files command metadata");
                application.commands().set_enabled_predicate(id, std::move(enabled));
                application.commands().set_command_scope(id, {.contexts = {"ck.files.pane"}});
                return id;
            };
            const auto row_ready = [this] { return availability("ck.files.toggle_mark").enabled; };
            const auto location_id =
                own("ck.files.location", [this] { show_location(); }, [this] { return can_edit_location(active); });
            const auto parent_id = own(
                "ck.files.parent",
                [this] {
                    navigate_relative(active, false);
                },
                [this] { return availability("ck.files.parent").enabled; });
            const auto root_id = own(
                "ck.files.root", [this] { navigate_relative(active, true); },
                [this] { return availability("ck.files.root").enabled; });
            const auto home_id =
                own("ck.files.home", [this] { navigate_home(active); },
                    [this] { return availability("ck.files.home").enabled; });
            history_back_id = own(
                "ck.files.history_back", [this] { navigate_history_back(active); },
                [this] { return availability("ck.files.history_back").enabled; });
            history_forward_id = own(
                "ck.files.history_forward", [this] { navigate_history_forward(active); },
                [this] { return availability("ck.files.history_forward").enabled; });
            bookmark_add_id = own(
                "ck.files.bookmark_add", [this] { add_bookmark(active); },
                [this] { return availability("ck.files.bookmark_add").enabled; });
            bookmark_remove_id = own(
                "ck.files.bookmark_remove", [this] { remove_bookmark(active); },
                [this] { return availability("ck.files.bookmark_remove").enabled; });
            refresh_id = own("ck.files.refresh", [this] { refresh(active); },
                             [this] { return availability("ck.files.refresh").enabled; });
            properties_id = own("ck.files.properties", [this] { show_properties(); },
                                [this] { return availability("ck.files.properties").enabled; });
            preview_id = own("ck.files.preview", [this] { show_preview(); },
                             [this] { return availability("ck.files.preview").enabled; });
            enter_id = own(
                "ck.files.enter_directory", [this] { enter_directory(active); },
                [this] { return availability("ck.files.enter_directory").enabled; });
            create_directory_id = own("ck.files.create_directory", [this] { show_create_directory(); },
                                      [this] { return availability("ck.files.create_directory").enabled; });
            rename_id = own("ck.files.rename", [this] { show_rename(); },
                            [this] { return availability("ck.files.rename").enabled; });
            trash_id = own("ck.files.trash", [this] { show_trash(); },
                           [this] { return availability("ck.files.trash").enabled; });
            delete_id = own("ck.files.permanently_delete", [this] { show_delete(); },
                            [this] { return availability("ck.files.permanently_delete").enabled; });
            copy_id = own("ck.files.copy", [this] { prepare_copy(ck::files::FileOperation::Copy); },
                          [this] { return availability("ck.files.copy").enabled; });
            move_id = own("ck.files.move", [this] { prepare_copy(ck::files::FileOperation::Move); },
                          [this] { return availability("ck.files.move").enabled; });
            archive_recovery_id = own("ck.files.inspect_archive_recovery", [this] { show_archive_recovery(); },
                                      [this] { return availability("ck.files.inspect_archive_recovery").enabled; });
            const auto visible_ready = [this] { return availability("ck.files.mark_visible").enabled; };
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
                [this] { return availability("ck.files.clear_marks").enabled; });
            const auto range = own(
                "ck.files.range_start",
                [this] {
                    if (const auto target = panes[active].model.target()) {
                        panes[active].range_start = target->row;
                        labels();
                    }
                },
                row_ready);
            const auto range_ready = [this] { return availability("ck.files.mark_range").enabled; };
            const auto mark_range = own("ck.files.mark_range", [this] { mark_through(true); }, range_ready);
            const auto unmark_range = own("ck.files.unmark_range", [this] { mark_through(false); }, range_ready);
            const auto options_id = own("ck.files.view_options", [this] { show_view_options(); },
                                        [this] { return availability("ck.files.view_options").enabled; });
            const auto item = [](auto id, const char *title) {
                return ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{id, title})
                    .with_disabled_reason("See the active pane status for why this command is unavailable.");
            };
            SuiteShellOptions options;
            options.application_name = "Files";
            options.about_text = "Two independent file locations with worker-owned folder creation.";
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
                     item(switch_id, "S&witch pane"), item(cancel_id, "&Cancel activity")}},
                ckv::widgets::MenuBarItem{"&Operations", {item(rename_id, "&Rename..."),
                                                            item(create_directory_id, "&New folder..."),
                                                            item(trash_id, "Move to &Trash / Recycle Bin..."),
                                                            item(delete_id, "&Delete permanently..."),
                                                            item(archive_recovery_id, "Inspect archive &recovery..."),
                                                            item(copy_id, "&Copy to other pane"),
                                                            item(move_id, "&Move to other pane")}},
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
                ckv::widgets::StatusLineItem{
                    ckv::widgets::CommandPresentation{archive_recovery_id, "&Inspect archive recovery"}, 24},
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
            if (volume_archive_recovery_service) {
                if (!operation_journal_root || !operation_journal_root->is_valid()) {
                    volume_archive_recovery_error = "archive journal is not configured";
                } else if (volume_archive_recovery_service->request(*operation_journal_root) ==
                           FilesVolumeArchiveRecoverySubmission::Accepted) {
                    volume_archive_recovery_request = true;
                } else {
                    volume_archive_recovery_error = "archive recovery scan could not be started";
                }
            }
            application.commands().set_handler(application.commands().standard().quit, [this] { request_close(); });
            for (std::size_t i = 0; i < 2; ++i) {
                panes[i].encoding = locations[i].encoding();
                if (ck::files::valid_pane_order({settings.value.views[i].sort, settings.value.views[i].descending,
                                                 settings.value.views[i].folders_first}))
                    panes[i].view = settings.value.views[i];
                for (auto &bookmark : settings.value.bookmarks[i])
                    panes[i].bookmarks.push_back(std::make_shared<const FilesPaneLocation>(std::move(bookmark)));
                navigate(i, FilesPaneLocation(std::move(locations[i])), panes[i].view);
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
        create_directory_dialog.reset();
        create_directory_window = nullptr;
        rename_dialog.reset();
        rename_window = nullptr;
        trash_dialog.reset();
        trash_window = nullptr;
        delete_dialog.reset();
        delete_window = nullptr;
        archive_recovery_dialog.reset();
        archive_recovery_window = nullptr;
        volume_request = false;
        operation_review_dialog.reset();
        operation_planning_request.reset();
        operation_execution_request.reset();
        operation_execution_kind.reset();
        operation_planning_encoding.reset();
        operation_planning_revalidation = {};
        operation_planning_completion = {};
        volume_archive_review_dialog.reset();
        volume_archive_planning_request.reset();
        volume_archive_execution_request.reset();
        volume_archive_destination.reset();
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
        if (planning_service)
            planning_service->request_stop();
        if (execution_service)
            execution_service->request_stop();
        if (volume_service)
            volume_service->request_stop();
        if (volume_archive_planning_service)
            volume_archive_planning_service->request_stop();
        if (volume_archive_service)
            volume_archive_service->request_stop();
        if (volume_archive_recovery_service)
            volume_archive_recovery_service->request_stop();
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
            surface->grid->on_changed = [this] {
                labels();
                wake.request();
            };
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
               (create_directory_dialog && !create_directory_dialog->completed()) ||
               (rename_dialog && !rename_dialog->completed()) ||
               (trash_dialog && !trash_dialog->completed()) ||
               (delete_dialog && !delete_dialog->completed()) ||
               (archive_recovery_dialog && !archive_recovery_dialog->completed()) ||
               volume_request ||
               (volume_archive_review_dialog && !volume_archive_review_dialog->completed()) ||
               volume_archive_planning_request.has_value() ||
               (operation_review_dialog && !operation_review_dialog->completed()) ||
               (location_dialog && !location_dialog->completed()) || operation_planning_request.has_value();
    }
    FilesCommandAvailability availability(std::string_view command) const {
        const auto unavailable = [](std::string reason) {
            return FilesCommandAvailability::unavailable(std::move(reason));
        };
        if (closing)
            return unavailable(std::string(command) + " unavailable: Files is closing");
        if (command == "ck.files.switch_pane")
            return modal_pending() ? unavailable("Switch pane unavailable: a dialog is open")
                                   : FilesCommandAvailability::available();
        if (command == "ck.files.cancel_read") {
            if (volume_archive_planning_request && volume_archive_planning_service)
                return FilesCommandAvailability::available();
            if (modal_pending())
                return unavailable("Cancel unavailable: a dialog is open");
            if (operation_execution_request || pending(panes[active]) || panes[active].volume_view)
                return FilesCommandAvailability::available();
            return unavailable("Cancel unavailable: no activity is running");
        }
        if (command == "ck.files.inspect_archive_recovery") {
            if (volume_archive_recovery_request)
                return unavailable("Recovery inspection unavailable: archive recovery scan is still running");
            if (!volume_archive_recovery_scan)
                return unavailable(volume_archive_recovery_error.empty()
                                       ? "Recovery inspection unavailable: archive recovery scan is not ready"
                                       : "Recovery inspection unavailable: " + volume_archive_recovery_error);
            return volume_archive_recovery_scan->journals.empty()
                       ? unavailable("Recovery inspection unavailable: no archive journals were found")
                       : FilesCommandAvailability::available();
        }
        if (command == "ck.files.parent") {
            if (panes[active].volume_view)
                return FilesCommandAvailability::available();
            if (!ready(active))
                return unavailable("Parent folder unavailable: active pane is not ready");
            if (!panes[active].active || !panes[active].active->parent().value())
                return unavailable("Parent folder unavailable: location has no parent");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.root") {
            if (!ready(active))
                return unavailable("Location root unavailable: active pane is not ready");
            if (!panes[active].active)
                return unavailable("Location root unavailable: no completed location");
            const auto root = panes[active].active->namespace_root();
            if (!root.value() || *root.value() == *panes[active].active)
                return unavailable("Location root unavailable: already at namespace root");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.location")
            return can_edit_location(active) ? FilesCommandAvailability::available()
                                             : unavailable("Location editor unavailable: active pane is busy");
        if (command == "ck.files.home")
            return ready(active) ? FilesCommandAvailability::available()
                                 : unavailable("Home location unavailable: active pane is not ready");
        if (command == "ck.files.history_back")
            return ready(active) && panes[active].history_position > 0
                       ? FilesCommandAvailability::available()
                       : unavailable("Go back unavailable: no earlier location is available");
        if (command == "ck.files.history_forward")
            return ready(active) && panes[active].history_position + 1 < panes[active].history.size()
                       ? FilesCommandAvailability::available()
                       : unavailable("Go forward unavailable: no later location is available");
        if (command == "ck.files.bookmark_add")
            return ready(active) && panes[active].active
                       ? FilesCommandAvailability::available()
                       : unavailable("Add bookmark unavailable: active location is not ready");
        if (command == "ck.files.bookmark_remove") {
            const bool present = panes[active].active &&
                                 std::any_of(panes[active].bookmarks.begin(), panes[active].bookmarks.end(),
                                             [this](const auto &bookmark) { return *bookmark == *panes[active].active; });
            return ready(active) && present ? FilesCommandAvailability::available()
                                            : unavailable("Remove bookmark unavailable: location is not bookmarked");
        }
        if (command == "ck.files.refresh")
            return ready(active) ? FilesCommandAvailability::available()
                                 : unavailable("Refresh unavailable: active pane is not ready");
        if (command == "ck.files.properties")
            return ready(active) && panes[active].model.cursor_row()
                       ? FilesCommandAvailability::available()
                       : unavailable("Properties unavailable: no current entry is selected");
        if (command == "ck.files.preview")
            return ready(active) && panes[active].model.cursor_kind() == ck::files::ObjectKind::RegularFile
                       ? FilesCommandAvailability::available()
                       : unavailable("Preview unavailable: select a regular file");
        if (command == "ck.files.enter_directory")
            return ready(active) && panes[active].model.cursor_kind() == ck::files::ObjectKind::Directory
                       ? FilesCommandAvailability::available()
                       : unavailable("Enter folder unavailable: select a folder");
        if (command == "ck.files.create_directory")
            return ready(active) ? FilesCommandAvailability::available()
                                 : unavailable("New folder unavailable: active pane is not ready");
        if (command == "ck.files.rename" || command == "ck.files.trash" ||
            command == "ck.files.permanently_delete") {
            if (panes[active].volume_view)
                return unavailable(std::string(command == "ck.files.rename" ? "Rename" :
                                                       command == "ck.files.trash" ? "Trash / Recycle Bin" : "Delete") +
                                   " unavailable: volume rows are archive sources, not entries");
            return ready(active) && panes[active].model.target()
                       ? FilesCommandAvailability::available()
                       : unavailable("Operation unavailable: select a current entry");
        }
        if (command == "ck.files.copy" || command == "ck.files.move") {
            const bool move = command == "ck.files.move";
            if (operation_execution_request || volume_archive_execution_request || volume_archive_planning_request ||
                (volume_archive_review_dialog && !volume_archive_review_dialog->completed()))
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: another operation is running");
            if (panes[active].volume_view) {
                if (move)
                    return unavailable("Move unavailable: volume rows are archive sources, not movable entries");
                const auto volume = panes[active].model.volume_cursor();
                if (!volume)
                    return unavailable("Copy unavailable: no volume is selected");
                if (volume->archive.support != FilesVolumeArchiveSupport::LiveReadable &&
                    volume->archive.support != FilesVolumeArchiveSupport::SnapshotPreferred)
                    return unavailable(volume->archive.support == FilesVolumeArchiveSupport::Denied
                                           ? "Copy unavailable: volume access is denied"
                                       : volume->archive.support == FilesVolumeArchiveSupport::Unsupported
                                           ? "Copy unavailable: volume archive format is unsupported"
                                           : "Copy unavailable: archive capability is unknown");
                if (!operation_journal_root || !operation_journal_root->is_valid())
                    return unavailable("Copy unavailable: archive journal is not configured");
                if (!volume_archive_planning_service || !volume_archive_service ||
                    !volume_archive_planning_service->running() || !volume_archive_service->running())
                    return unavailable("Copy unavailable: archive worker is unavailable");
                if (!volume_archive_recovery_service)
                    return unavailable("Copy unavailable: archive recovery service is not configured");
                if (volume_archive_recovery_request)
                    return unavailable("Copy unavailable: archive recovery scan is still running");
                if (!volume_archive_recovery_error.empty())
                    return unavailable("Copy unavailable: " + volume_archive_recovery_error);
                if (!volume_archive_recovery_scan)
                    return unavailable("Copy unavailable: archive recovery scan is not ready");
                if (volume_archive_recovery_scan->requires_reconciliation)
                    return unavailable("Copy unavailable: archive recovery review is required");
                if (!archive_destination_ready(active))
                    return unavailable("Copy unavailable: destination pane is not ready");
                return FilesCommandAvailability::available();
            }
            if (!execution_service || !operation_journal_root)
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: operation worker is not configured");
            if (!ready(active) || !ready(1 - active))
                return unavailable(std::string(move ? "Move" : "Copy") +
                                   " unavailable: both panes must be ready");
            if (!panes[active].model.target())
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: no entry is selected");
            const auto kind = panes[active].model.cursor_kind();
            return kind == ck::files::ObjectKind::RegularFile || kind == ck::files::ObjectKind::Directory
                       ? FilesCommandAvailability::available()
                       : unavailable(std::string(move ? "Move" : "Copy") + " unavailable: selected row is not a file or folder");
        }
        if (command == "ck.files.toggle_mark" || command == "ck.files.range_start")
            return ready(active) && panes[active].model.target() && !panes[active].volume_view
                       ? FilesCommandAvailability::available()
                       : unavailable("Mark unavailable: volume rows and empty selections cannot be marked");
        if (command == "ck.files.mark_visible" || command == "ck.files.unmark_visible" ||
            command == "ck.files.invert_marks")
            return ready(active) && panes[active].model.row_count() != 0 && !panes[active].volume_view
                       ? FilesCommandAvailability::available()
                       : unavailable("Marking unavailable: no file rows are visible");
        if (command == "ck.files.clear_marks")
            return ready(active) && panes[active].selection.marks && panes[active].selection.marks->total != 0
                       ? FilesCommandAvailability::available()
                       : unavailable("Clear marks unavailable: no entries are marked");
        if (command == "ck.files.mark_range" || command == "ck.files.unmark_range")
            return ready(active) && panes[active].range_start && panes[active].model.target()
                       ? FilesCommandAvailability::available()
                       : unavailable("Range marking unavailable: set a range start and select an entry");
        if (command == "ck.files.view_options")
            return ready(active) ? FilesCommandAvailability::available()
                                 : unavailable("View options unavailable: active pane is not ready");
        return FilesCommandAvailability::available();
    }
    bool can_edit_location(std::size_t i) const {
        return i < 2 && !closing && !modal_pending() && services[i]->running();
    }
    static bool pending(const Pane &p) {
        return p.navigating || p.reading || p.changing || p.queued_view || p.previewing || p.mutating;
    }
    bool ready(std::size_t i, bool continuation = false) const {
        return i < 2 && !closing && !operation_execution_request && (continuation || !modal_pending()) &&
               !pending(panes[i]) && !panes[i].volume_view && panes[i].active &&
               panes[i].model.current() && services[i]->running();
    }
    bool archive_destination_ready(std::size_t source_pane) const {
        if (source_pane >= 2 || !volume_archive_planning_service || !volume_archive_service ||
            !volume_archive_planning_service->running() || !volume_archive_service->running())
            return false;
        const auto destination_pane = 1 - source_pane;
        const auto &destination = panes[destination_pane];
        return !destination.volume_view && !pending(destination) && destination.active &&
               destination.model.current() && services[destination_pane]->running();
    }
    std::string status(std::size_t i) const {
        const auto &p = panes.at(i);
        std::string text;
        if (volume_archive_planning_request)
            text = "Planning volume archive...";
        else if (volume_archive_execution_request)
            text = "Creating volume archive...";
        else if (operation_execution_request)
            text = operation_execution_kind && *operation_execution_kind == ck::files::FileOperation::Move
                ? "Moving to the other pane..." : "Copying to the other pane...";
        else if (p.volume_view) {
            const auto volume = p.model.volume_cursor();
            if (volume)
                text = (volume->read_only ? "RO" : "RW") + std::string(" | ") +
                       (volume->mount_point.empty() ? volume->name : volume->mount_point) + " | " +
                       (volume->filesystem.empty() ? "Unknown" : volume->filesystem) + " | " +
                       volume_bytes(volume->free_bytes) + " free / " + volume_bytes(volume->total_bytes);
            else
                text = "No visible volumes";
        }
        else if (p.cancelling)
            text = "Cancelling activity...";
        else if (p.navigating)
            text = p.entering ? "Opening folder..." : p.homing ? "Opening home location..." : "Opening location...";
        else if (p.preparing_cursor)
            text = "Reading current row for view change...";
        else if (p.previewing)
            text = "Reading preview...";
        else if (p.mutating)
            text = p.renaming ? "Renaming..." : p.trashing ? "Moving to Trash / Recycle Bin..."
                              : p.deleting ? "Deleting permanently..." : "Creating folder...";
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
        std::string hint;
        if (p.volume_view) {
            // A volume row has one meaningful operation: archive Copy. Keep
            // its reason visible beside the row so the disabled menu, palette
            // and keyboard command never leave the user guessing.
            hint = availability("ck.files.copy").reason;
        } else if (i == active && volume_archive_recovery_scan &&
                   volume_archive_recovery_scan->requires_reconciliation) {
            hint = "Inspect archive recovery: review is required before volume Copy";
        } else if (i == active && !closing && !modal_pending() && !pending(p)) {
            // The status-line commands are deliberately ordered by immediate
            // relevance. Report only the first unavailable contextual action;
            // listing every disabled command would turn ordinary browsing
            // into a stream of boilerplate. A selected ordinary row gets the
            // operation reason first so a disabled Copy/Move caused by the
            // other pane being busy is visible without opening a menu.
            static constexpr std::string_view empty_listing_contextual[] = {
                "ck.files.enter_directory", "ck.files.parent", "ck.files.toggle_mark",
                "ck.files.cancel_read", "ck.files.view_options"};
            static constexpr std::string_view selected_contextual[] = {
                "ck.files.copy", "ck.files.move"};
            const auto report_first_unavailable = [&](const auto &contextual) {
                for (const auto command : contextual) {
                    const auto candidate = availability(command);
                    if (!candidate.enabled) {
                        hint = candidate.reason;
                        break;
                    }
                }
            };
            if (p.model.target())
                report_first_unavailable(selected_contextual);
            else
                report_first_unavailable(empty_listing_contextual);
        }
        if (!hint.empty())
            text += "\nHint: " + hint;
        if (!settings.diagnostic.empty())
            text += " | " + settings.diagnostic;
        return text;
    }
    void show_archive_recovery() {
        if (!availability("ck.files.inspect_archive_recovery").enabled ||
            !volume_archive_recovery_scan || modal_pending())
            return;
        std::ostringstream summary;
        summary << "Read-only inspection. No archive is resumed, deleted, replaced, or retried.\n\n";
        constexpr std::size_t max_items = 64;
        const auto &journals = volume_archive_recovery_scan->journals;
        const auto count = std::min(max_items, journals.size());
        for (std::size_t index = 0; index != count; ++index) {
            const auto &item = journals[index];
            summary << "Journal: " << item.display_name << '\n';
            if (item.report) {
                const auto &report = *item.report;
                summary << "Source: " << report.evidence.source_mount_point << '\n'
                        << "Archive: " << report.evidence.archive_name << '\n'
                        << "Transaction: " << report.evidence.transaction_id << '\n';
                if (report.evidence.destination_encoding) {
                    summary << "Destination: exact native path recorded ("
                            << (*report.evidence.destination_encoding == ck::files::NameEncoding::PosixBytes
                                    ? "POSIX bytes" : "Windows UTF-16")
                            << ")\n";
                } else {
                    summary << "Destination: not recorded in this legacy journal\n";
                }
                for (const auto &recovery : report.operation.items)
                    summary << "State: " << archive_recovery_state(recovery.state)
                            << "; action: " << archive_recovery_action(recovery.action) << '\n';
                if (report.operation.has_untrusted_tail)
                    summary << "Journal tail: untrusted\n";
            } else if (item.error) {
                summary << "Inspection error: " << item.error->error.operation << '\n';
            }
            summary << '\n';
        }
        if (journals.size() > count)
            summary << "Additional journal entries omitted from this bounded view: "
                    << (journals.size() - count) << '\n';
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Archive recovery review";
        descriptor.resizable = true;
        descriptor.fields = {ckv::widgets::FieldDescriptor{.label = summary.str(),
                                                            .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Close", ckv::widgets::ButtonRole::Dismiss, {}}};
        archive_recovery_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        archive_recovery_window = shell->desktop().active_window();
        archive_recovery_lifetime = archive_recovery_window->lifetime_token();
        archive_recovery_dialog->set_completion_handler([this](const ckv::widgets::DialogResult &) {
            archive_recovery_window = nullptr;
            archive_recovery_dialog.reset();
            labels();
        });
    }
    void save_settings() {
        if (!settings.save)
            return;
        FilesPersistentSettings value;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            value.views[i] = panes[i].view;
            value.bookmarks[i].reserve(panes[i].bookmarks.size());
            for (const auto &bookmark : panes[i].bookmarks)
                value.bookmarks[i].push_back(*bookmark);
        }
        settings.diagnostic = settings.save(value);
        labels();
    }
    void labels() {
        if (!window)
            return;
        for (std::size_t i = 0; i < 2; ++i) {
            auto &p = panes[i];
            const std::string side = i == 0 ? "Left" : "Right";
            auto location = p.active ? p.active->display_text() : std::string("No completed location");
            auto title = (active == i ? "[Active] " : "[Other] ") + side + "\n" +
                         (p.volume_view ? "Volumes and mounts above " + location : location);
            if (surfaces[i]->heading->text() != title)
                surfaces[i]->heading->set_text(std::move(title));
            auto text = status(i);
            if (p.navigating && p.requested)
                text += "\n" + p.requested->display_text();
            if (surfaces[i]->footer->text() != text)
                surfaces[i]->footer->set_text(std::move(text));
        }
        const auto pane_items = [](const Pane &pane) {
            if (!pane.model.current())
                return pending(pane) ? std::string("loading") : std::string("stale");
            return std::to_string(pane.model.row_count());
        };
        const auto pane_marks = [](const Pane &pane) {
            return pane.selection.marks ? pane.selection.marks->total : std::size_t{0};
        };
        std::string activity;
        if (volume_archive_execution_request)
            activity = "Archiving volume";
        else if (operation_execution_request)
            activity = operation_execution_kind && *operation_execution_kind == ck::files::FileOperation::Move
                ? "Moving" : "Copying";
        else if (modal_pending())
            activity = "Dialog open";
        else if (pending(panes[0]) || pending(panes[1]))
            activity = "Scanning";
        else
            activity = "Ready";
        window->set_footer("Active: " + std::string(active == 0 ? "Left" : "Right") +
                           " | Items: L " + pane_items(panes[0]) + " / R " + pane_items(panes[1]) +
                           " | Marked: L " + std::to_string(pane_marks(panes[0])) +
                           " / R " + std::to_string(pane_marks(panes[1])) + " | " + activity);
    }
    bool navigate(std::size_t i, ck::files::NativeAbsolutePath location) {
        if (i >= 2 || closing || !location.is_valid())
            return false;
        return navigate(i, FilesPaneLocation(std::move(location)), {});
    }
    bool show_volume_info(std::size_t i) {
        if (i >= 2 || closing || !volume_service || !volume_service->running() || !panes[i].active ||
            !panes[i].active->components().empty() || panes[i].volume_view || volume_request || modal_pending())
            return false;
        if (!volume_service->request()) {
            panes[i].error = "Volume information is busy";
            labels();
            return false;
        }
        volume_pane = i;
        volume_request = true;
        labels();
        return true;
    }
    void consume_volume(FilesVolumeReply reply) {
        if (!volume_request)
            return;
        const auto pane = volume_pane;
        volume_request = false;
        if (reply.state != FilesVolumeReplyState::Ready) {
            panes[pane].error = volume_failure(reply);
            labels();
            return;
        }
        panes[pane].volume_view = true;
        panes[pane].model.publish_volumes(std::move(reply.volumes));
        panes[pane].error.clear();
        application.set_focus(surfaces[pane]->grid);
        labels();
    }
    bool relative_ready(std::size_t i, bool root) const {
        if (!ready(i) || !panes[i].active)
            return false;
        const auto next = root ? panes[i].active->namespace_root() : panes[i].active->parent();
        if (!next.value())
            return false;
        if (*next.value() == *panes[i].active)
            return !root && panes[i].active->components().empty() && volume_service && volume_service->running();
        return true;
    }
    bool navigate_relative(std::size_t i, bool root) {
        if (i < 2 && !root && panes[i].volume_view) {
            panes[i].volume_view = false;
            panes[i].model.clear();
            return navigate(i, *panes[i].active, panes[i].view);
        }
        if (!relative_ready(i, root))
            return false;
        auto next = root ? panes[i].active->namespace_root() : panes[i].active->parent();
        if (!root && next.value() && *next.value() == *panes[i].active)
            return show_volume_info(i);
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
        save_settings();
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
        if (panes[i].bookmarks.size() != before)
            save_settings();
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
    bool create_directory(std::size_t i, ck::files::NativeName name) {
        if (!ready(i) || !name.is_valid())
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->create_directory(p.navigation, p.stamp, std::move(name),
                                                              {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Folder creation request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.cancelling = false;
        p.error.clear();
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool rename(std::size_t i, FilesPaneRowTarget target, ck::files::NativeName name) {
        if (!ready(i) || !name.is_valid())
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->rename(p.navigation, target.stamp, target.row, std::move(name),
                                                    {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Rename request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.renaming = true;
        p.cancelling = false;
        p.error.clear();
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool trash(std::size_t i, FilesPaneRowTarget target) {
        if (!ready(i))
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->trash(p.navigation, target.stamp, target.row,
                                                   {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Trash / Recycle Bin request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.renaming = false;
        p.trashing = true;
        p.deleting = false;
        p.cancelling = false;
        p.error.clear();
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool permanently_delete(std::size_t i, FilesPaneRowTarget target) {
        if (!ready(i))
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->permanently_delete(p.navigation, target.stamp, target.row,
                                                               {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Permanent deletion request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.renaming = false;
        p.trashing = false;
        p.deleting = true;
        p.cancelling = false;
        p.error.clear();
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
    void show_create_directory() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto encoding = panes[pane].encoding;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Create folder";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "&Name:",
                .validate = [encoding](const std::string &value) {
                    return parse_directory_component(value, encoding).has_value();
                },
                .description = encoding == ck::files::NameEncoding::WindowsUtf16
                                   ? "Strict UTF-8 input; names that cannot be represented as Windows UTF-16 are refused."
                                   : "One exact native component; separators, dot names and empty input are refused."},
            ckv::widgets::FieldDescriptor{.label = "Creation is exclusive and is performed relative to the held pane directory.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Create", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        create_directory_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        create_directory_window = shell->desktop().active_window();
        create_directory_lifetime = create_directory_window->lifetime_token();
        create_directory_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, encoding](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                create_directory_window = nullptr;
                if (!result.accepted) {
                    create_directory_dialog.reset();
                    return;
                }
                if (result.values.size() != 2 || panes[pane].navigation != navigation) {
                    panes[pane].error = "Location changed while folder creation was open";
                    create_directory_dialog.reset();
                    labels();
                    return;
                }
                auto name = parse_directory_component(result.values[0], encoding);
                create_directory_dialog.reset();
                if (!name || !create_directory(pane, std::move(*name))) {
                    panes[pane].error = "Folder name is not representable or the location changed";
                    labels();
                }
            });
    }
    std::optional<ck::files::NativeAbsolutePath> native_location_address(const FilesPaneLocation &location) const {
        auto address = location.root_address();
        for (const auto &component : location.components()) {
            auto child = address.child_address(component);
            if (!child.value())
                return {};
            address = std::move(child).take_value();
        }
        return address;
    }
    void prepare_volume_archive() {
        if (!archive_destination_ready(active) || !panes[active].volume_view ||
            !volume_archive_planning_service)
            return;
        const auto volume = panes[active].model.volume_cursor();
        const auto destination_pane = 1 - active;
        const auto destination_location = panes[destination_pane].active;
        if (!volume || !destination_location)
            return;
        const auto destination = native_location_address(*destination_location);
        if (!destination) {
            panes[active].error = "Volume archive unavailable: destination address is not representable";
            labels();
            return;
        }
        const auto source = *volume;
        auto build_document = [source](std::stop_token token) {
            return build_native_volume_archive_document(source, FilesVolumeArchiveConsistency::Live, token);
        };
        FilesVolumeArchivePlanningRequest request{
            source, *destination, FilesVolumeArchiveReview::default_archive_name(source.name),
            FilesVolumeArchiveConsistency::Live, std::move(build_document)};
        const auto submitted = volume_archive_planning_service->request(std::move(request));
        if (submitted.state != FilesVolumeArchivePlanningSubmission::Accepted) {
            panes[active].error = "Volume archive planning worker is unavailable";
            labels();
            return;
        }
        volume_archive_source = source;
        volume_archive_destination = destination_location;
        volume_archive_planning_request = submitted.ticket;
        panes[active].error.clear();
        labels();
    }
    bool can_prepare_copy() const {
        if (panes[active].volume_view)
            return archive_destination_ready(active) && panes[active].model.volume_cursor().has_value();
        if (!execution_service || !operation_journal_root || active > 1 ||
            operation_execution_request || !ready(active) || !ready(1 - active))
            return false;
        if (!panes[active].model.target().has_value())
            return false;
        const auto kind = panes[active].model.cursor_kind();
        return kind == ck::files::ObjectKind::RegularFile || kind == ck::files::ObjectKind::Directory;
    }
    void prepare_copy(ck::files::FileOperation operation) {
        if (operation == ck::files::FileOperation::Copy && panes[active].volume_view) {
            prepare_volume_archive();
            return;
        }
        if ((operation != ck::files::FileOperation::Copy && operation != ck::files::FileOperation::Move) ||
            !can_prepare_copy())
            return;
        const auto source_pane = active;
        const auto destination_pane = 1 - active;
        const auto source_navigation = panes[source_pane].navigation;
        const auto destination_navigation = panes[destination_pane].navigation;
        const auto source_stamp = panes[source_pane].stamp;
        const auto destination_stamp = panes[destination_pane].stamp;
        FilesOperationPanePlanningRequest planning;
        planning.operation = operation;
        planning.source_service = services[source_pane];
        planning.destination_service = services[destination_pane];
        planning.source_navigation = source_navigation;
        planning.destination_navigation = destination_navigation;
        planning.source_stamp = source_stamp;
        planning.destination_stamp = destination_stamp;
        planning.source_limits.access = ck::files::EntryAccess::ContentRead;
        planning.evidence_limits.max_items = planning.source_limits.held_entries;
        planning.projection_policy.require_unhandled_metadata_consent = true;
        const auto revalidate = [this, source_pane, destination_pane, source_navigation, destination_navigation](
                                    const ck::files::ConsentReceipt &receipt) {
            if (receipt.action != ck::files::ConsentAction::Proceed &&
                receipt.action != ck::files::ConsentAction::ContinueWithListedLosses)
                return ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                    ck::files::ObservationState::Unsupported,
                    {ck::files::ErrorDomain::Adapter,
                     static_cast<std::int64_t>(ck::files::AdapterError::ProviderReviewRequired),
                     "files_app.copy_decision"});
            if (panes[source_pane].navigation != source_navigation || panes[destination_pane].navigation != destination_navigation ||
                !panes[source_pane].stamp.current() || !panes[destination_pane].stamp.current())
                return ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                    ck::files::ObservationState::Changed,
                    {ck::files::ErrorDomain::Adapter,
                     static_cast<std::int64_t>(ck::files::AdapterError::PlanDecisionStale),
                     "files_app.copy_panes_changed"});
            return ck::files::Observation<ck::files::ConsentReceipt>::available(receipt);
        };
        const auto completion = [this, operation, source_pane, destination_pane, source_navigation, destination_navigation,
                                 source_stamp, destination_stamp](
                                    ck::files::Observation<ck::files::ConsentReceipt> result) mutable {
            if (!result.value()) {
                panes[source_pane].error = std::string(operation == ck::files::FileOperation::Move ? "Move" : "Copy") +
                    " was not authorized: " +
                    (result.unavailable_detail() ? result.unavailable_detail()->error.operation : "review failed");
                labels();
                return;
            }
            ck::files::OperationJournalId journal_id;
            const auto serial = operation_serial++;
            for (unsigned byte = 0; byte != sizeof(serial); ++byte)
                journal_id.bytes[byte] = std::byte{static_cast<unsigned char>(serial >> (byte * 8U))};
            FilesOperationExecutionRequest execution{
                operation, services[source_pane], services[destination_pane], source_navigation, destination_navigation,
                source_stamp, destination_stamp, {}, *operation_journal_root, journal_id, {}, {}};
            execution.source_limits.held_entries = 1024;
            execution.source_limits.access = ck::files::EntryAccess::ContentRead;
            execution.policy.allow_unhandled_metadata_loss =
                result.value()->action == ck::files::ConsentAction::ContinueWithListedLosses;
            execution.open_journal = [](const ck::files::NativeAbsolutePath &root,
                                        const ck::files::NativeName &basename, bool create_new) {
                return ck::files::open_native_operation_journal_file(root, basename, create_new);
            };
            const auto submitted = execution_service->request(std::move(execution));
            if (submitted.state != FilesOperationExecutionSubmission::Accepted) {
                panes[source_pane].error = std::string(operation == ck::files::FileOperation::Move ? "Move" : "Copy") +
                    " worker is unavailable";
                labels();
                return;
            }
            operation_execution_request = submitted.ticket;
            operation_execution_kind = operation;
            panes[source_pane].error.clear();
            panes[destination_pane].error.clear();
            labels();
        };
        if (!prepare_operation(std::move(planning), panes[destination_pane].encoding,
                               revalidate, completion)) {
            panes[source_pane].error = operation == ck::files::FileOperation::Move
                ? "Move planning is unavailable" : "Copy planning is unavailable";
            labels();
        }
    }
    void show_rename() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto target = panes[pane].model.target();
        const auto encoding = panes[pane].encoding;
        if (!target)
            return;
        const auto row = target->row;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Rename entry";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "&New name:",
                .validate = [encoding](const std::string &value) {
                    return parse_directory_component(value, encoding).has_value();
                },
                .description = encoding == ck::files::NameEncoding::WindowsUtf16
                                   ? "Strict UTF-8 input; unrepresentable Windows UTF-16 names are refused."
                                   : "One exact native component; separators, dot names and empty input are refused."},
            ckv::widgets::FieldDescriptor{.label = "The worker will revalidate the selected entry identity before renaming.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Rename", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        rename_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        rename_window = shell->desktop().active_window();
        rename_lifetime = rename_window->lifetime_token();
        rename_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, row, encoding](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                rename_window = nullptr;
                if (!result.accepted) {
                    rename_dialog.reset();
                    return;
                }
                const auto current = panes[pane].model.target();
                if (result.values.size() != 2 || panes[pane].navigation != navigation || !current || current->row != row) {
                    panes[pane].error = "Entry changed while rename was open";
                    rename_dialog.reset();
                    labels();
                    return;
                }
                auto name = parse_directory_component(result.values[0], encoding);
                rename_dialog.reset();
                if (!name || !rename(pane, *current, std::move(*name))) {
                    panes[pane].error = "Entry name is not representable or the entry changed";
                    labels();
                }
            });
    }
    void show_trash() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto target = panes[pane].model.target();
        if (!target)
            return;
        const auto row = target->row;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Move entry to Trash / Recycle Bin";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "The platform Trash / Recycle Bin will receive the selected entry when supported. No permanent-delete fallback is used.",
                .kind = ckv::widgets::FieldKind::Note},
            ckv::widgets::FieldDescriptor{.label = "The worker will revalidate the selected entry identity before moving it.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Move to Trash", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        trash_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        trash_window = shell->desktop().active_window();
        trash_lifetime = trash_window->lifetime_token();
        trash_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, row](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                trash_window = nullptr;
                if (!result.accepted) {
                    trash_dialog.reset();
                    return;
                }
                const auto current = panes[pane].model.target();
                if (panes[pane].navigation != navigation || !current || current->row != row) {
                    panes[pane].error = "Entry changed while Trash / Recycle Bin was open";
                    trash_dialog.reset();
                    labels();
                    return;
                }
                trash_dialog.reset();
                if (!trash(pane, *current)) {
                    panes[pane].error = "Trash / Recycle Bin request was rejected";
                    labels();
                }
            });
    }
    void show_delete() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto target = panes[pane].model.target();
        if (!target)
            return;
        const auto row = target->row;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Delete entry permanently";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "This is irreversible. The selected entry will be permanently deleted; it will not be moved to Trash or the Recycle Bin.",
                .kind = ckv::widgets::FieldKind::Note},
            ckv::widgets::FieldDescriptor{.label = "The worker will revalidate the selected entry identity before deletion.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Delete permanently", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        delete_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop(), shell->roles()));
        delete_window = shell->desktop().active_window();
        delete_lifetime = delete_window->lifetime_token();
        delete_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, row](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                delete_window = nullptr;
                if (!result.accepted) {
                    delete_dialog.reset();
                    return;
                }
                const auto current = panes[pane].model.target();
                if (panes[pane].navigation != navigation || !current || current->row != row) {
                    panes[pane].error = "Entry changed while permanent deletion was open";
                    delete_dialog.reset();
                    labels();
                    return;
                }
                delete_dialog.reset();
                if (!permanently_delete(pane, *current)) {
                    panes[pane].error = "Permanent deletion request was rejected";
                    labels();
                }
            });
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
        const bool pending = operation_planning_request.has_value() || volume_request ||
            volume_archive_planning_request.has_value() || volume_archive_execution_request.has_value() ||
            std::any_of(panes.begin(), panes.end(), [](const Pane &p) { return State::pending(p); });
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
        if (reply.kind == FilesPaneReplyKind::Mutation) {
            if (reply.navigation != p.navigation || reply.request != p.request)
                return;
            p.cancelling = false;
            p.mutating = false;
            const bool was_rename = p.renaming;
            const bool was_trash = p.trashing;
            const bool was_delete = p.deleting;
            p.renaming = false;
            p.trashing = false;
            p.deleting = false;
            p.model.set_input_locked(false);
            if (reply.state == FilesPaneReplyState::Ready && reply.mutation_applied.value_or(false)) {
                p.error.clear();
                // The old page is not a current listing after a successful
                // mutation. Reopen the retained route through the worker so
                // the UI never presents a stale post-mutation page.
                if (!refresh(i))
                    p.error = was_delete ? "Entry permanently deleted; refresh could not be started"
                                         : was_trash ? "Entry moved to Trash / Recycle Bin; refresh could not be started"
                                         : was_rename ? "Entry renamed; refresh could not be started"
                                                      : "Directory created; refresh could not be started";
            } else {
                p.error = reply.state == FilesPaneReplyState::Ready
                              ? (was_delete ? "Permanent deletion outcome unavailable"
                                            : was_trash ? "Trash / Recycle Bin outcome unavailable"
                                            : was_rename ? "Rename outcome unavailable"
                                                         : "Directory creation outcome unavailable")
                              : failure(reply);
            }
            labels();
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
                if (reply.kind == FilesPaneReplyKind::View && reply.change_outcome == FilesPaneChangeOutcome::Applied)
                    save_settings();
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
    void consume_volume_archive_plan(FilesVolumeArchivePlanningReply reply) {
        if (!volume_archive_planning_request || reply.request != *volume_archive_planning_request)
            return;
        volume_archive_planning_request.reset();
        if (reply.state != FilesVolumeArchivePlanningState::Ready || !reply.review) {
            panes[active].error = "Volume archive planning failed";
            if (reply.error)
                panes[active].error += ": " + reply.error->error.operation;
            labels();
            return;
        }
        const auto review = std::move(reply.review);
        const auto source_pane = active;
        const auto destination_pane = 1 - source_pane;
        volume_archive_review_dialog = std::make_unique<FilesVolumeArchiveReviewDialog>(
            application, shell->desktop(), shell->roles(), review,
            [this, weak = std::weak_ptr<void>(lifetime), review, source_pane, destination_pane](
                ck::files::Observation<FilesVolumeArchiveApproval> result) mutable {
                if (weak.expired())
                    return;
                volume_archive_review_dialog.reset();
                if (!result.value()) {
                    panes[source_pane].error = "Volume archive cancelled";
                    labels();
                    return;
                }
                const auto current_volume = panes[source_pane].model.volume_cursor();
                if (!panes[source_pane].volume_view || !current_volume ||
                    current_volume->identity != volume_archive_source.identity ||
                    current_volume->mount_point != volume_archive_source.mount_point ||
                    !volume_archive_destination || !panes[destination_pane].active ||
                    *volume_archive_destination != *panes[destination_pane].active) {
                    panes[source_pane].error = "Volume archive review became stale; start a new review";
                    labels();
                    return;
                }
                if (!volume_archive_service || !volume_archive_service->running()) {
                    panes[source_pane].error = "Volume archive worker is unavailable";
                    labels();
                    return;
                }
                auto review_document = review->document();
                review_document.losses_reviewed = review_document.losses.empty() ||
                    result.value()->continued_with_listed_losses;
                auto build_document = [archive_document = std::move(review_document)](std::stop_token token) mutable {
                    if (token.stop_requested())
                        return ck::files::Observation<FilesVolumeArchiveDocument>::unavailable(
                            ck::files::ObservationState::Unknown,
                            {ck::files::ErrorDomain::Adapter,
                             static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                             "volume_archive.cancelled"});
                    return ck::files::Observation<FilesVolumeArchiveDocument>::available(archive_document);
                };
                ck::files::OperationJournalId journal_id;
                const auto serial = operation_serial++;
                for (unsigned byte = 0; byte != sizeof(serial); ++byte)
                    journal_id.bytes[byte] = std::byte{static_cast<unsigned char>(serial >> (byte * 8U))};
                FilesVolumeArchiveRequest request{
                    volume_archive_source, review->destination_directory(), review->archive_name(),
                    review->document().consistency, serial, std::move(build_document),
                    std::optional<ck::files::NativeAbsolutePath>{*operation_journal_root},
                    journal_id, {},
                    [](const ck::files::NativeAbsolutePath &root, const ck::files::NativeName &basename,
                       bool create_new) {
                        return ck::files::open_native_operation_journal_file(root, basename, create_new);
                    }};
                const auto submitted = volume_archive_service->request(std::move(request));
                if (submitted.state != FilesVolumeArchiveSubmission::Accepted) {
                    panes[source_pane].error = "Volume archive worker rejected the approved review";
                    labels();
                    return;
                }
                volume_archive_execution_request = submitted.ticket;
                panes[source_pane].error.clear();
                labels();
            });
        labels();
    }
    void consume_volume_archive(FilesVolumeArchiveReply reply) {
        if (!volume_archive_execution_request || reply.request != *volume_archive_execution_request)
            return;
        volume_archive_execution_request.reset();
        if (reply.state == FilesVolumeArchiveState::Complete ||
            reply.state == FilesVolumeArchiveState::Indeterminate) {
            const auto destination_pane = 1 - active;
            const auto &result = reply.result;
            std::string message = reply.state == FilesVolumeArchiveState::Complete
                                      ? "Volume archive created"
                                      : "Volume archive published; durability is indeterminate";
            if (result)
                message += ": " + result->published_path.string();
            panes[destination_pane].error = std::move(message);
        } else {
            panes[active].error = reply.state == FilesVolumeArchiveState::Cancelled
                                      ? "Volume archive cancelled"
                                      : "Volume archive failed";
            if (reply.error)
                panes[active].error += ": " + reply.error->error.operation;
        }
        volume_archive_destination.reset();
        labels();
    }
    void consume_planning(FilesOperationPlanningReply reply) {
        if (!operation_planning_request || reply.request != *operation_planning_request)
            return;
        const auto encoding = *operation_planning_encoding;
        auto revalidate = std::move(operation_planning_revalidation);
        auto completion = std::move(operation_planning_completion);
        operation_planning_request.reset();
        operation_planning_encoding.reset();
        operation_planning_revalidation = {};
        operation_planning_completion = {};
        if (reply.state == FilesOperationPlanningReplyState::Ready && reply.review) {
            const auto fallback = completion;
            if (!review_operation(std::move(reply.review), encoding, std::move(revalidate), std::move(completion)))
                fallback(ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                    ck::files::ObservationState::Failed,
                    {ck::files::ErrorDomain::Adapter,
                        static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                        "files_app.operation_review_start"}));
            return;
        }
        const auto detail = reply.error.value_or(ck::files::UnavailableObservation{
            ck::files::ObservationState::Failed,
            {ck::files::ErrorDomain::Adapter,
                static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                "files_app.operation_planning"}});
        completion(ck::files::Observation<ck::files::ConsentReceipt>::unavailable(detail.state, detail.error));
    }
    void consume_execution(FilesOperationExecutionReply reply) {
        if (!operation_execution_request || reply.request != *operation_execution_request)
            return;
        operation_execution_request.reset();
        if (reply.state == FilesOperationExecutionState::Complete) {
            const auto source = active;
            const auto destination = 1 - active;
            const bool source_refreshed = refresh(source);
            const bool destination_refreshed = refresh(destination);
            if (!source_refreshed || !destination_refreshed) {
                panes[source].error = std::string(*operation_execution_kind == ck::files::FileOperation::Move ? "Move" : "Copy") +
                    " completed; refresh could not be started";
                panes[destination].error = panes[source].error;
            }
        } else {
            const auto operation = operation_execution_kind.value_or(ck::files::FileOperation::Copy);
            std::string detail = reply.state == FilesOperationExecutionState::Cancelled
                ? (operation == ck::files::FileOperation::Move ? "Move cancelled" : "Copy cancelled")
                : (operation == ck::files::FileOperation::Move ? "Move failed" : "Copy failed");
            if (reply.error)
                detail += ": " + reply.error->error.operation;
            else if (reply.worker_fault != FilesPaneWorkerFault::None)
                detail += ": worker stopped";
            panes[active].error = std::move(detail);
        }
        operation_execution_kind.reset();
        labels();
    }
    void pump() {
        if (closing || pumping)
            return;
        pumping = true;
        try {
            if (planning_service) {
                if (auto reply = planning_service->take())
                    consume_planning(std::move(*reply));
                if (operation_planning_request && !planning_service->running()) {
                    auto completion = std::move(operation_planning_completion);
                    operation_planning_request.reset();
                    operation_planning_encoding.reset();
                    operation_planning_revalidation = {};
                    operation_planning_completion = {};
                    completion(ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                        ck::files::ObservationState::Failed,
                        {ck::files::ErrorDomain::Adapter,
                            static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                            "files_app.operation_planning_worker"}));
                }
            }
            if (volume_archive_planning_service) {
                if (auto reply = volume_archive_planning_service->take())
                    consume_volume_archive_plan(std::move(*reply));
                if (volume_archive_planning_request && !volume_archive_planning_service->running()) {
                    volume_archive_planning_request.reset();
                    panes[active].error = "Volume archive planning worker stopped";
                }
            }
            if (volume_archive_recovery_service) {
                if (auto reply = volume_archive_recovery_service->take()) {
                    volume_archive_recovery_request = false;
                    if (reply->state == FilesVolumeArchiveRecoveryState::Ready && reply->scan) {
                        volume_archive_recovery_scan = std::move(reply->scan);
                        volume_archive_recovery_error.clear();
                    } else {
                        volume_archive_recovery_scan.reset();
                        volume_archive_recovery_error = reply->error
                            ? "archive recovery scan failed: " + reply->error->error.operation
                            : "archive recovery scan failed";
                    }
                    labels();
                }
                if (volume_archive_recovery_request && !volume_archive_recovery_service->running()) {
                    volume_archive_recovery_request = false;
                    volume_archive_recovery_error = "archive recovery worker stopped";
                    labels();
                }
            }
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
                    panes[i].mutating = panes[i].renaming = panes[i].trashing = false;
                    panes[i].changing.reset();
                    panes[i].queued_view.reset();
                    panes[i].model.set_input_locked(false);
                    panes[i].error = changing ? "Worker stopped; change outcome unknown" : "Worker stopped";
                }
                page(i);
            }
            if (execution_service) {
                if (auto reply = execution_service->take())
                    consume_execution(std::move(*reply));
                if (operation_execution_request && !execution_service->running()) {
                    operation_execution_request.reset();
                    panes[active].error = operation_execution_kind &&
                            *operation_execution_kind == ck::files::FileOperation::Move
                        ? "Move worker stopped" : "Copy worker stopped";
                    labels();
                }
            }
            if (volume_archive_service) {
                if (auto reply = volume_archive_service->take())
                    consume_volume_archive(std::move(*reply));
                if (volume_archive_execution_request && !volume_archive_service->running()) {
                    volume_archive_execution_request.reset();
                    panes[active].error = "Volume archive worker stopped";
                    labels();
                }
            }
            if (volume_service) {
                if (auto reply = volume_service->take())
                    consume_volume(std::move(*reply));
                if (volume_request && !volume_service->running()) {
                    volume_request = false;
                    panes[volume_pane].error = "Volume information worker stopped";
                }
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
        if (!closing && volume_archive_planning_request && volume_archive_planning_service) {
            volume_archive_planning_service->cancel(*volume_archive_planning_request);
            labels();
            return;
        }
        if (!closing && !modal_pending() && operation_execution_request && execution_service) {
            execution_service->cancel(*operation_execution_request);
            labels();
            return;
        }
        if (!closing && !modal_pending() && volume_archive_execution_request && volume_archive_service) {
            volume_archive_service->cancel(*volume_archive_execution_request);
            labels();
            return;
        }
        if (!closing && !modal_pending() && panes[active].volume_view) {
            (void)navigate_relative(active, false);
            return;
        }
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
        if (planning_service)
            planning_service->request_stop();
        if (execution_service)
            execution_service->request_stop();
        if (volume_service)
            volume_service->request_stop();
        if (volume_archive_planning_service)
            volume_archive_planning_service->request_stop();
        if (volume_archive_service)
            volume_archive_service->request_stop();
        if (volume_archive_recovery_service)
            volume_archive_recovery_service->request_stop();
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
        if (create_directory_dialog && !create_directory_dialog->completed()) {
            if (create_directory_window && !create_directory_lifetime.expired())
                create_directory_window->close();
            return;
        }
        if (rename_dialog && !rename_dialog->completed()) {
            if (rename_window && !rename_lifetime.expired())
                rename_window->close();
            return;
        }
        if (trash_dialog && !trash_dialog->completed()) {
            if (trash_window && !trash_lifetime.expired())
                trash_window->close();
            return;
        }
        if (delete_dialog && !delete_dialog->completed()) {
            if (delete_window && !delete_lifetime.expired())
                delete_window->close();
            return;
        }
        if (archive_recovery_dialog && !archive_recovery_dialog->completed()) {
            if (archive_recovery_window && !archive_recovery_lifetime.expired())
                archive_recovery_window->close();
            return;
        }
        if (operation_review_dialog && !operation_review_dialog->completed()) {
            operation_review_dialog->close();
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

    bool review_operation(ck::files::OperationReviewModel::Pointer model,
        ck::files::NameEncoding destination_encoding, FilesApp::OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion) {
        if (closing || modal_pending() || !model || !revalidate || !completion)
            return false;
        const auto weak = std::weak_ptr<void>(lifetime);
        operation_review_dialog = std::make_unique<FilesOperationReviewDialog>(
            application, shell->desktop(), shell->roles(), std::move(model), destination_encoding,
            [this, weak, review_revalidation = std::move(revalidate), delivered_completion = std::move(completion)](
                ck::files::Observation<ck::files::ConsentReceipt> result) mutable {
                if (weak.expired())
                    return;
                operation_review_dialog.reset();
                if (result.value() && result.value()->action != ck::files::ConsentAction::Cancel &&
                    result.value()->action != ck::files::ConsentAction::ChooseAnotherDestination) {
                    try {
                        result = review_revalidation(*result.value());
                    } catch (...) {
                        result = ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                            ck::files::ObservationState::Failed,
                            {ck::files::ErrorDomain::Adapter,
                                static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                                "files_app.operation_revalidation"});
                    }
                }
                delivered_completion(std::move(result));
            });
        return true;
    }
    bool prepare_operation(FilesOperationPlanningRequest request, ck::files::NameEncoding destination_encoding,
        FilesApp::OperationRevalidation revalidate, FilesOperationReviewDialog::Completion completion) {
        if (closing || modal_pending() || !planning_service || !revalidate || !completion ||
            !planning_service->running())
            return false;
        const auto submitted = planning_service->request(std::move(request));
        if (submitted.state != FilesOperationPlanningSubmission::Accepted)
            return false;
        operation_planning_request = submitted.ticket;
        operation_planning_encoding = destination_encoding;
        operation_planning_revalidation = std::move(revalidate);
        operation_planning_completion = std::move(completion);
        labels();
        return true;
    }
    bool prepare_operation(FilesOperationPanePlanningRequest request,
        ck::files::NameEncoding destination_encoding, FilesApp::OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion) {
        if (closing || modal_pending() || !planning_service || !revalidate || !completion ||
            !planning_service->running())
            return false;
        const auto submitted = planning_service->request(std::move(request));
        if (submitted.state != FilesOperationPlanningSubmission::Accepted)
            return false;
        operation_planning_request = submitted.ticket;
        operation_planning_encoding = destination_encoding;
        operation_planning_revalidation = std::move(revalidate);
        operation_planning_completion = std::move(completion);
        labels();
        return true;
    }
};
FilesApp::FilesApp(ckv::ui::Application &application, std::array<FilesPaneService *, 2> services, FilesAppWake &wake,
                   std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> host_quit,
                   ThemeScheme theme, std::array<FilesCompletionService *, 2> completion_services,
                   FilesOperationPlanningService *planning_service,
                   FilesOperationExecutionService *execution_service,
                   std::shared_ptr<const ck::files::NativeAbsolutePath> operation_journal_root,
                   FilesSettingsBinding settings, FilesVolumeService *volume_service,
                   FilesVolumeArchivePlanningService *volume_archive_planning_service,
                   FilesVolumeArchiveService *volume_archive_service,
                   FilesVolumeArchiveRecoveryService *volume_archive_recovery_service)
    : state_(std::make_unique<State>(application, services, wake, std::move(locations), std::move(host_quit), theme,
                                     completion_services, planning_service, execution_service,
                                     std::move(operation_journal_root), std::move(settings), volume_service,
                                     volume_archive_planning_service, volume_archive_service,
                                     volume_archive_recovery_service)) {}
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
bool FilesApp::create_directory(std::size_t pane, ck::files::NativeName name) {
    return state_->create_directory(pane, std::move(name));
}
bool FilesApp::rename(std::size_t pane, ck::files::NativeName name) {
    const auto target = state_->panes.at(pane).model.target();
    if (!target)
        return false;
    return state_->rename(pane, *target, std::move(name));
}
bool FilesApp::trash(std::size_t pane) {
    const auto target = state_->panes.at(pane).model.target();
    if (!target)
        return false;
    return state_->trash(pane, *target);
}
bool FilesApp::permanently_delete(std::size_t pane) {
    const auto target = state_->panes.at(pane).model.target();
    if (!target)
        return false;
    return state_->permanently_delete(pane, *target);
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
bool FilesApp::operation_review_pending() const {
    return state_->operation_review_dialog && !state_->operation_review_dialog->completed();
}
bool FilesApp::location_editor_pending() const {
    return state_->location_dialog && !state_->location_dialog->completed();
}
bool FilesApp::volume_info_pending() const {
    return state_->volume_request;
}
bool FilesApp::review_operation(ck::files::OperationReviewModel::Pointer model,
    ck::files::NameEncoding destination_encoding, OperationRevalidation revalidate,
    FilesOperationReviewDialog::Completion completion) {
    return state_->review_operation(std::move(model), destination_encoding, std::move(revalidate),
        std::move(completion));
}
bool FilesApp::prepare_operation(FilesOperationPlanningRequest request, ck::files::NameEncoding destination_encoding,
    OperationRevalidation revalidate, FilesOperationReviewDialog::Completion completion) {
    return state_->prepare_operation(std::move(request), destination_encoding, std::move(revalidate),
        std::move(completion));
}
bool FilesApp::prepare_operation(FilesOperationPanePlanningRequest request,
    ck::files::NameEncoding destination_encoding, OperationRevalidation revalidate,
    FilesOperationReviewDialog::Completion completion) {
    return state_->prepare_operation(std::move(request), destination_encoding, std::move(revalidate),
        std::move(completion));
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
FilesCommandAvailability FilesApp::availability(std::string_view command) const {
    return state_->availability(command);
}
std::optional<FilesAppTarget> FilesApp::target(std::size_t pane) const {
    const auto &p = state_->panes.at(pane);
    if (state_->closing || p.navigating || p.changing || p.queued_view || p.previewing || p.mutating ||
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
