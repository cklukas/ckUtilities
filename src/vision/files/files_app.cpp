// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "files_compare_sync_service.hpp"
#include "files_embedded_editor.hpp"
#include "files_location_dialog.hpp"
#include "files_native_io_helpers.hpp"
#include "files_ordinary_archive.hpp"
#include "files_remote_provider.hpp"
#include "files_search_service.hpp"
#include "files_settings_service.hpp"
#include "files_terminal_session.hpp"
#include "files_volume_archive_restore.hpp"
#include "files_volume_source.hpp"
#include "ck/vision/keymap.hpp"
#include "name_mapping.hpp"
#include "unicode_scalar.hpp"
#include <cvision/core/text.hpp>
#include <cvision/ui/layout.hpp>
#include <cvision/widgets/command_palette.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/editor_document.hpp>
#include <cvision/widgets/list_view.hpp>
#include <cvision/widgets/mnemonic.hpp>
#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/terminal_view.hpp>
#include <cvision/widgets/text_editor.hpp>
#include <cvision/widgets/text_view.hpp>
#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>
#include <stdexcept>
#if defined(_WIN32)
#include <winerror.h>
#endif
namespace ck::vision {
namespace {
bool archive_storage_exhausted(ck::files::ErrorDomain domain, std::int64_t code) noexcept {
    if (domain == ck::files::ErrorDomain::Posix) {
        return code == ENOSPC
#if defined(EDQUOT)
            || code == EDQUOT
#endif
            ;
    }
#if defined(_WIN32)
    return domain == ck::files::ErrorDomain::Win32 &&
           (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL || code == ERROR_DISK_QUOTA_EXCEEDED);
#else
    return false;
#endif
}

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

class ArchiveRecoveryList final : public ckv::widgets::ListModel {
  public:
    explicit ArchiveRecoveryList(std::shared_ptr<const FilesVolumeArchiveRecoveryScan> scan)
        : scan_(std::move(scan)), order_(scan_->journals.size()) {
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        const auto terminal = std::stable_partition(order_.begin(), order_.end(), [this](std::size_t index) {
            const auto &journal = scan_->journals[index];
            return !journal.report || journal.report->operation.requires_reconciliation;
        });
        review_count_ = static_cast<std::size_t>(terminal - order_.begin());
    }
    std::size_t item_count() const override { return order_.size(); }
    std::size_t review_count() const { return review_count_; }
    std::optional<std::size_t> index_of(ckv::widgets::ListItemId id) const override {
        return id > 0 && id <= item_count() ? std::optional<std::size_t>(id - 1) : std::nullopt;
    }
    ckv::widgets::ListItem item_at(std::size_t index) const override {
        const auto &journal = item(index);
        if (!journal.report)
            return {index + 1, "[Journal error] " + journal.display_name};
        const auto &operation = journal.report->operation;
        const auto state = operation.requires_reconciliation ? "Review" :
            archive_recovery_state(operation.items.front().state);
        const std::string_view name = journal.report->evidence.archive_name;
        constexpr std::size_t max_label_bytes = 256;
        return {index + 1, std::string("[") + state + "] " +
            ck::files::display_native_text(name.substr(0, max_label_bytes)) +
            (name.size() > max_label_bytes ? "..." : "")};
    }
    std::string details(std::size_t index) const {
        const auto &journal = item(index);
        std::ostringstream text;
        text << "Journal: " << journal.display_name << '\n';
        if (!journal.report) {
            if (journal.error)
                text << "Inspection error: " << ck::files::display_native_text(journal.error->error.operation) << '\n';
            return text.str();
        }
        const auto &report = *journal.report;
        for (const auto &recovery : report.operation.items)
            text << "State: " << archive_recovery_state(recovery.state)
                 << "\nAction: " << archive_recovery_action(recovery.action) << '\n';
        if (report.operation.has_untrusted_tail)
            text << "Journal tail: untrusted\n";
        text << "Archive: " << ck::files::display_native_text(report.evidence.archive_name) << '\n'
             << "Transaction: " << report.evidence.transaction_id << '\n'
             << "Recorded source: " << ck::files::display_native_text(report.evidence.source_mount_point) << '\n';
        if (const auto destination = report.evidence.display_destination_folder())
            text << "Recorded destination folder: " << *destination << '\n';
        else
            text << "Recorded destination folder unavailable\n";
        return text.str();
    }

  private:
    const FilesVolumeArchiveRecoveryItem &item(std::size_t index) const {
        return scan_->journals[order_[index]];
    }
    std::shared_ptr<const FilesVolumeArchiveRecoveryScan> scan_;
    std::vector<std::size_t> order_;
    std::size_t review_count_ = 0;
};

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0, t = 0;
    std::size_t star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

class OperationRecoveryList final : public ckv::widgets::ListModel {
  public:
    explicit OperationRecoveryList(std::shared_ptr<const FilesOperationRecoveryScan> scan)
        : scan_(std::move(scan)) {}
    std::size_t item_count() const override { return scan_ ? scan_->journals.size() : 0; }
    std::optional<std::size_t> index_of(ckv::widgets::ListItemId id) const override {
        return id > 0 && id <= item_count() ? std::optional<std::size_t>(id - 1) : std::nullopt;
    }
    ckv::widgets::ListItem item_at(std::size_t index) const override {
        const auto &item = scan_->journals[index];
        const char *tag = item.kind == FilesOperationJournalKind::RetainedTarget ? "Retained" : "Transfer";
        if (item.error)
            return {index + 1, std::string("[") + tag + " Error] " + item.display_name};
        std::string state = "Review";
        if (item.report && !item.report->inspection.report.items.empty())
            state = archive_recovery_state(item.report->inspection.report.items.front().state);
        return {index + 1, std::string("[") + tag + ": " + state + "] " + item.display_name};
    }
    std::string details(std::size_t index) const {
        const auto &item = scan_->journals[index];
        std::ostringstream out;
        out << "Journal: " << item.display_name << '\n';
        const bool retention = item.kind == FilesOperationJournalKind::RetainedTarget;
        if (retention)
            out << "Previous target version\n";
        if (item.report) {
            const auto &inspection = item.report->inspection;
            out << "Items: " << inspection.report.items.size() << '\n';
            for (const auto &recovery : inspection.report.items) {
                out << "State: " << archive_recovery_state(recovery.state)
                    << "; action: " << archive_recovery_action(recovery.action) << '\n';
                for (const auto &art : inspection.artifacts) {
                    if (art.item == recovery.item && art.evidence) {
                        const auto &entry = *art.evidence;
                        if (retention) {
                            out << "Original target: " << entry.source_name.value_or(entry.staging_name).display_name() << '\n'
                                << "Retained name: " << entry.destination_name.display_name() << '\n';
                            if (entry.destination_address)
                                out << "Recorded folder: " << entry.destination_address->display_text() << '\n';
                        } else if (entry.source_name) {
                            out << "Source: " << entry.source_name->display_name() << '\n'
                                << "Destination: " << entry.destination_name.display_name() << '\n';
                            if (entry.destination_address)
                                out << "Recorded folder: " << entry.destination_address->display_text() << '\n';
                        }
                        break;
                    }
                }
            }
            if (inspection.report.has_untrusted_tail)
                out << "Journal tail: untrusted\n";
            if (inspection.has_untrusted_evidence)
                out << "Native evidence: review required\n";
        } else if (item.error) {
            out << "Inspection error: " << item.error->error.operation << '\n';
        }
        return out.str();
    }
    const FilesOperationRecoveryItem &journal(std::size_t index) const { return scan_->journals[index]; }

  private:
    std::shared_ptr<const FilesOperationRecoveryScan> scan_;
};

std::optional<std::string> preview_utf8(const FilesPanePreview &preview) {
    std::string text;
    if (preview.bytes.empty())
        return text;
    std::string_view bytes(reinterpret_cast<const char *>(preview.bytes.data()), preview.bytes.size());
    if (bytes.starts_with("\xef\xbb\xbf"))
        bytes.remove_prefix(3);
    text.reserve(bytes.size());
    while (!bytes.empty()) {
        const auto scalar = ck::files::detail::decode_utf8_scalar(bytes);
        if (scalar.units == 0)
            return std::nullopt;
        const auto encoded = bytes.substr(0, scalar.units);
        bytes.remove_prefix(scalar.units);
        if (scalar.value == '\r') {
            if (bytes.starts_with('\n'))
                bytes.remove_prefix(1);
            text += '\n';
        } else if (scalar.value == '\n') {
            text += '\n';
        } else if (scalar.value == '\t') {
            text += "    ";
        } else {
            if (!ckv::text::is_sanitized_display_text(encoded) || scalar.value == 0x061c ||
                scalar.value == 0x200e || scalar.value == 0x200f ||
                (scalar.value >= 0x202a && scalar.value <= 0x202e) ||
                (scalar.value >= 0x2066 && scalar.value <= 0x2069))
                return std::nullopt;
            text += encoded;
        }
    }
    return text;
}
std::string preview_hex(const FilesPanePreview &preview) {
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
    return out.str();
}
std::string link_text(const ck::files::LinkMetadata &link) {
    std::ostringstream out;
    if (const auto *posix = std::get_if<ck::files::PosixSymlinkData>(&link.payload)) {
        out << "POSIX symbolic-link target\n" << ck::files::display_native_text(posix->target);
    } else if (const auto *windows = std::get_if<ck::files::WindowsReparseData>(&link.payload)) {
        const char *kind = windows->kind == ck::files::WindowsReparseKind::SymbolicLink ? "symbolic link"
                           : windows->kind == ck::files::WindowsReparseKind::MountPoint ? "mount point"
                           : windows->kind == ck::files::WindowsReparseKind::OpaqueGuid ? "opaque GUID reparse"
                                                                                         : "opaque Microsoft reparse";
        out << "Windows reparse kind: " << kind << "\n"
            << "Tag: 0x" << std::hex << windows->tag << std::dec << "\n";
        if (windows->names) {
            out << "Substitute name: " << ck::files::display_native_text(windows->names->substitute) << '\n'
                << "Print name: " << ck::files::display_native_text(windows->names->print) << '\n'
                << "Flags: 0x" << std::hex << windows->names->flags << std::dec;
        } else {
            out << "Opaque payload: " << windows->raw.size() << " bytes (not interpreted as a target)";
        }
    } else {
        out << "No link payload was returned";
    }
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
std::optional<ck::files::SymbolicLinkTarget> parse_symbolic_link_target(std::string text,
                                                                         ck::files::NameEncoding encoding) {
    if (encoding != ck::files::NameEncoding::PosixBytes || text.empty() || text.find('\0') != std::string::npos)
        return std::nullopt;
    return ck::files::SymbolicLinkTarget{std::move(text)};
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
    struct RowInspection {
        FilesPaneTicket ticket;
        FilesPaneReplyKind kind;
    };
    struct Pane {
        FilesPaneGridModel model;
        FilesPaneTicket navigation, request;
        ck::files::PaneStateStamp stamp;
        std::shared_ptr<const FilesPaneLocation> active, requested;
        std::optional<bool> read_only_mount;
        std::optional<FilesPaneViewport> cache, blocked;
        FilesPaneViewport inflight;
        bool navigating = false, reading = false, navigation_error = false, entering = false, homing = false;
        bool refreshing = false, refresh_failed = false, queued_refresh = false;
        std::optional<ck::files::FileOperation> queued_copy;
        bool change_error = false, cancelling = false, preparing_cursor = false, mutating = false, renaming = false,
             deleting = false, trashing = false, creating_file = false, creating_hard_link = false,
             creating_symbolic_link = false;
        std::optional<FilesPaneReplyKind> changing;
        std::vector<ck::files::IndexedMarkCommand> queued_marks;
        enum class BatchDeleteKind { None, Trash, PermanentDelete };
        BatchDeleteKind batch_delete_kind = BatchDeleteKind::None;
        std::vector<std::string> batch_delete_names;
        std::optional<ck::files::PaneView> queued_view;
        ck::files::PaneView view;
        std::vector<std::shared_ptr<const FilesPaneLocation>> history;
        std::size_t history_position = 0;
        std::optional<std::size_t> history_target;
        std::vector<std::shared_ptr<const FilesPaneLocation>> bookmarks;
        std::optional<RowInspection> inspection;
        ck::files::NameEncoding encoding = ck::files::NameEncoding::PosixBytes;
        ck::files::IndexedPaneSelection selection;
        std::optional<std::uint64_t> worker_focus;
        std::optional<ck::files::PaneRowId> range_start;
        std::string error;
        std::string archive_notice;
        std::optional<FilesPaneTicket> archive_refresh;
        bool volume_view = false, volume_inventory_stale = false;
    };
    struct VolumeArchiveOrigin {
        std::size_t source_pane;
        FilesPaneTicket source_navigation;
        FilesPaneTicket destination_navigation;
        FilesVolumeRecord volume;
        std::shared_ptr<const FilesPaneLocation> destination;
    };
    struct OperationExecutionOrigin {
        ck::files::FileOperation kind;
        std::size_t source_pane;
        FilesPaneTicket source_navigation;
        FilesPaneTicket destination_navigation;
    };
    struct VolumeRequest {
        std::size_t pane;
        FilesPaneTicket navigation;
        bool retired = false;
    };
    ckv::ui::Application &application;
    std::array<FilesPaneService *, 2> services;
    std::array<FilesCompletionService *, 2> completion_services;
    FilesOperationPlanningService *planning_service = nullptr;
    FilesOperationExecutionService *execution_service = nullptr;
    FilesVolumeService *volume_service = nullptr;
    FilesVolumeArchiveService *volume_archive_service = nullptr;
    FilesVolumeArchiveRecoveryService *volume_archive_recovery_service = nullptr;
    FilesVolumeArchiveRecoveryCleanupService *volume_archive_recovery_cleanup_service = nullptr;
    FilesOperationRecoveryService *operation_recovery_service = nullptr;
    FilesOperationRecoveryCleanupService *operation_recovery_cleanup_service = nullptr;
    std::shared_ptr<const ck::files::NativeAbsolutePath> operation_journal_root;
    std::shared_ptr<const ck::files::NativeAbsolutePath> volume_archive_scratch_root;
    FilesSettingsBinding settings;
    std::uint64_t settings_revision = 0;
    bool settings_waiting = false, settings_feedback_current = false;
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
    std::optional<ckv::widgets::DescriptorDialogPresentation> bookmark_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> link_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> create_directory_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> create_file_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> create_hard_link_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> create_symbolic_link_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> rename_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> trash_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> delete_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> archive_cleanup_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> operation_recovery_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> operation_recovery_cleanup_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> mark_pattern_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> parity_dialog;
    ckv::widgets::Window *parity_report_window = nullptr;
    std::weak_ptr<void> parity_report_lifetime;
    struct EmbeddedEditorInstance final {
        ckv::widgets::Window *window = nullptr;
        ckv::widgets::TextEditor *editor = nullptr;
        std::weak_ptr<void> lifetime;
        std::shared_ptr<ckv::widgets::EditorDocument> document;
        FilesDocumentSnapshot snapshot;
        bool allow_discard_close = false;
    };
    enum class ActiveViewMode { Pane, Editor, PreviewText, PreviewHex, PreviewHexOnly, Terminal, Report };
    ActiveViewMode active_view_mode = ActiveViewMode::Pane;
    std::vector<ckv::widgets::MenuBarItem> pane_menus;
    ckv::ui::CommandId editor_save_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_undo_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_redo_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_cut_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_copy_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_paste_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_find_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_find_next_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_wrap_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_line_numbers_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId editor_close_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_show_hex_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_show_text_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_close_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId terminal_focus_files_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId terminal_close_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId report_close_id = ckv::ui::kInvalidCommand;
    std::function<void()> preview_toggle_action;
    bool preview_is_hex = false;
    bool preview_can_toggle = false;
    std::unique_ptr<ckv::widgets::SyntaxProfileRegistry> syntax_registry;
    std::vector<EmbeddedEditorInstance> embedded_editors;
    ckv::widgets::Window *embedded_terminal_window = nullptr;
    std::weak_ptr<void> embedded_terminal_lifetime;
    FilesCommandHistory command_history;
    std::uint64_t external_writer_epoch = 0;
    std::unique_ptr<FilesOperationReviewDialog> operation_review_dialog;
    std::optional<ckv::widgets::DescriptorDialogPresentation> tree_copy_dialog;
    ckv::widgets::Window *tree_copy_window = nullptr;
    std::weak_ptr<void> tree_copy_window_lifetime;
    std::optional<FilesOperationPlanningTicket> operation_planning_request;
    bool operation_planning_cancelling = false;
    std::optional<FilesOperationExecutionTicket> operation_execution_request;
    std::optional<FilesVolumeArchivePlanningTicket> volume_archive_planning_request;
    bool volume_archive_planning_cancelling = false;
    std::optional<FilesVolumeArchiveTicket> volume_archive_execution_request;
    bool volume_archive_recovery_request = false;
    std::shared_ptr<const FilesVolumeArchiveRecoveryScan> volume_archive_recovery_scan;
    std::string volume_archive_recovery_error;
    bool volume_archive_recovery_cleanup_request = false;
    std::string volume_archive_recovery_cleanup_error;
    bool operation_recovery_request = false;
    bool operation_recovery_dirty = false;
    std::optional<FilesOperationRecoveryScan> operation_recovery_scan;
    std::string operation_recovery_error;
    bool operation_recovery_cleanup_request = false;
    std::string operation_recovery_cleanup_error;
    std::unique_ptr<FilesVolumeArchiveReviewDialog> volume_archive_review_dialog;
    std::optional<VolumeArchiveOrigin> volume_archive_origin;
    std::optional<OperationExecutionOrigin> operation_execution_origin;
    std::optional<ck::files::NameEncoding> operation_planning_encoding;
    FilesApp::OperationRevalidation operation_planning_revalidation;
    using PlanningCompletion = std::function<void(ck::files::Observation<ck::files::ConsentReceipt>,
        FilesOperationReviewEvidence::Pointer)>;
    PlanningCompletion operation_planning_completion;
    std::optional<std::pair<std::size_t, FilesOperationPanePlanningRequest>> operation_planning_transfer;
    std::unique_ptr<FilesLocationDialog> location_dialog;
    bool close_after_location = false;
    ckv::widgets::Window *view_dialog_window = nullptr;
    std::weak_ptr<void> view_dialog_lifetime;
    bool close_after_view = false;
    ckv::widgets::Window *bookmark_dialog_window = nullptr;
    std::weak_ptr<void> bookmark_dialog_lifetime;
    bool close_after_bookmark = false;
    ckv::widgets::Window *properties_dialog_window = nullptr;
    std::weak_ptr<void> properties_dialog_lifetime;
    ckv::widgets::Window *preview_dialog_window = nullptr;
    std::weak_ptr<void> preview_dialog_lifetime;
    ckv::widgets::Window *link_dialog_window = nullptr;
    std::weak_ptr<void> link_dialog_lifetime;
    ckv::widgets::Window *create_directory_window = nullptr;
    std::weak_ptr<void> create_directory_lifetime;
    ckv::widgets::Window *create_file_window = nullptr;
    std::weak_ptr<void> create_file_lifetime;
    ckv::widgets::Window *create_hard_link_window = nullptr;
    std::weak_ptr<void> create_hard_link_lifetime;
    ckv::widgets::Window *create_symbolic_link_window = nullptr;
    std::weak_ptr<void> create_symbolic_link_lifetime;
    ckv::widgets::Window *rename_window = nullptr;
    std::weak_ptr<void> rename_lifetime;
    ckv::widgets::Window *trash_window = nullptr;
    std::weak_ptr<void> trash_lifetime;
    ckv::widgets::Window *delete_window = nullptr;
    std::weak_ptr<void> delete_lifetime;
    ckv::widgets::Window *archive_recovery_window = nullptr;
    std::weak_ptr<void> archive_recovery_lifetime;
    ckv::widgets::Window *archive_cleanup_window = nullptr;
    std::weak_ptr<void> archive_cleanup_lifetime;
    ckv::widgets::Window *operation_recovery_window = nullptr;
    std::weak_ptr<void> operation_recovery_lifetime;
    ckv::widgets::Window *operation_recovery_cleanup_window = nullptr;
    std::weak_ptr<void> operation_recovery_cleanup_lifetime;
    ckv::widgets::Window *reconciliation_overlap_window = nullptr;
    std::weak_ptr<void> reconciliation_overlap_lifetime;
    std::optional<VolumeRequest> volume_request;
    ckv::ui::CommandId switch_id = ckv::ui::kInvalidCommand, cancel_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId highlighted_menu_command = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId pressed_footer_command = ckv::ui::kInvalidCommand;
    std::size_t last_input_mouse_count = 0;
    bool palette_focused = false;
    ckv::ui::CommandId enter_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId create_directory_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId create_file_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId create_hard_link_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId create_symbolic_link_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId rename_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId trash_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId delete_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId copy_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId move_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_recovery_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_cleanup_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId operation_recovery_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId operation_recovery_cleanup_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId resume_recovery_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId retry_recovery_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId restore_retained_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId dispose_retained_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId swap_panes_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId sync_other_pane_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId compare_directories_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId mark_pattern_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId unmark_pattern_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId search_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId directory_tree_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId sync_directories_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId diff_files_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_browse_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_create_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_extract_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId edit_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId new_edit_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId terminal_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId run_command_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId user_menu_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId connect_remote_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId inspect_volume_archive_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId restore_volume_archive_id = ckv::ui::kInvalidCommand;
    bool operation_recovery_action_pending = false;
    std::string operation_recovery_action_status;
    ckv::ui::CommandId history_back_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId history_forward_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bookmark_add_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bookmark_remove_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId refresh_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId properties_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_id = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId inspect_link_id = ckv::ui::kInvalidCommand;
    SuiteCommandScope commands;
    State(ckv::ui::Application &app, std::array<FilesPaneService *, 2> supplied, FilesAppWake &signal,
          std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> quit, ThemeScheme theme,
          std::array<FilesCompletionService *, 2> supplied_completion, FilesOperationPlanningService *planning,
          FilesOperationExecutionService *execution,
          std::shared_ptr<const ck::files::NativeAbsolutePath> journal_root, FilesSettingsBinding supplied_settings,
          FilesVolumeService *volumes,
          FilesVolumeArchiveService *archive_service, FilesVolumeArchiveRecoveryService *archive_recovery,
          FilesVolumeArchiveRecoveryCleanupService *archive_recovery_cleanup,
          FilesOperationRecoveryService *operation_recovery,
          FilesOperationRecoveryCleanupService *operation_recovery_cleanup,
          std::shared_ptr<const ck::files::NativeAbsolutePath> archive_scratch_root)
        : application(app), services(supplied), completion_services(supplied_completion), planning_service(planning),
          execution_service(execution), volume_service(volumes),
          volume_archive_service(archive_service),
          volume_archive_recovery_service(archive_recovery),
          volume_archive_recovery_cleanup_service(archive_recovery_cleanup),
          operation_recovery_service(operation_recovery),
          operation_recovery_cleanup_service(operation_recovery_cleanup),
          operation_journal_root(std::move(journal_root)),
          volume_archive_scratch_root(std::move(archive_scratch_root)),
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
            const auto bookmark_open_id = own(
                "ck.files.bookmark_open", [this] { show_bookmarks(); },
                [this] { return availability("ck.files.bookmark_open").enabled; });
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
            const auto copy_path_id = own("ck.files.copy_path", [this] { copy_path(); },
                                          [this] { return availability("ck.files.copy_path").enabled; });
            preview_id = own("ck.files.preview", [this] { show_preview(); },
                             [this] { return availability("ck.files.preview").enabled; });
            inspect_link_id = own("ck.files.inspect_link", [this] { show_inspect_link(); },
                                  [this] { return availability("ck.files.inspect_link").enabled; });
            enter_id = own(
                "ck.files.enter_directory", [this] {
                    if (panes[active].model.cursor_kind() == ck::files::ObjectKind::Directory)
                        enter_directory(active);
                    else
                        show_preview();
                },
                [this] { return availability("ck.files.enter_directory").enabled; });
            create_directory_id = own("ck.files.create_directory", [this] { show_create_directory(); },
                                      [this] { return availability("ck.files.create_directory").enabled; });
            create_file_id = own("ck.files.create_file", [this] { show_create_file(); },
                                 [this] { return availability("ck.files.create_file").enabled; });
            create_hard_link_id = own("ck.files.create_hard_link", [this] { show_create_hard_link(); },
                                      [this] { return availability("ck.files.create_hard_link").enabled; });
            create_symbolic_link_id = own("ck.files.create_symbolic_link", [this] { show_create_symbolic_link(); },
                                          [this] { return availability("ck.files.create_symbolic_link").enabled; });
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
            archive_cleanup_id = own("ck.files.cleanup_archive_recovery", [this] { show_archive_cleanup(); },
                                     [this] { return availability("ck.files.cleanup_archive_recovery").enabled; });
            operation_recovery_id = own("ck.files.inspect_operation_recovery", [this] { show_operation_recovery(); },
                                       [this] { return availability("ck.files.inspect_operation_recovery").enabled; });
            operation_recovery_cleanup_id = own(
                "ck.files.cleanup_operation_recovery", [this] { show_operation_recovery_cleanup(); },
                [this] { return availability("ck.files.cleanup_operation_recovery").enabled; });
            resume_recovery_id = own(
                "ck.files.resume_operation_recovery",
                [this] { execute_first_recovery_action(FilesOperationRecoveryActionKind::ResumeRecordedJob); },
                [this] { return availability("ck.files.resume_operation_recovery").enabled; });
            retry_recovery_id = own(
                "ck.files.retry_operation_recovery",
                [this] { execute_first_recovery_action(FilesOperationRecoveryActionKind::RetryUnresolvedItems); },
                [this] { return availability("ck.files.retry_operation_recovery").enabled; });
            restore_retained_id = own(
                "ck.files.restore_retained_target",
                [this] { execute_first_recovery_action(FilesOperationRecoveryActionKind::RestoreRetainedTarget); },
                [this] { return availability("ck.files.restore_retained_target").enabled; });
            dispose_retained_id = own(
                "ck.files.dispose_retained_target",
                [this] { execute_first_recovery_action(FilesOperationRecoveryActionKind::DisposeRetainedTarget); },
                [this] { return availability("ck.files.dispose_retained_target").enabled; });
            swap_panes_id = own("ck.files.swap_panes", [this] { swap_panes(); },
                                [this] { return availability("ck.files.swap_panes").enabled; });
            sync_other_pane_id = own("ck.files.sync_other_pane", [this] { sync_other_pane(); },
                                     [this] { return availability("ck.files.sync_other_pane").enabled; });
            compare_directories_id = own("ck.files.compare_directories", [this] { compare_directories(); },
                                         [this] { return availability("ck.files.compare_directories").enabled; });
            mark_pattern_id = own("ck.files.mark_pattern", [this] { show_mark_pattern(true); },
                                  [this] { return availability("ck.files.mark_pattern").enabled; });
            unmark_pattern_id = own("ck.files.unmark_pattern", [this] { show_mark_pattern(false); },
                                    [this] { return availability("ck.files.unmark_pattern").enabled; });
            search_id = own("ck.files.search", [this] { show_search(); },
                            [this] { return availability("ck.files.search").enabled; });
            directory_tree_id = own("ck.files.directory_tree", [this] { show_directory_tree(); },
                                    [this] { return availability("ck.files.directory_tree").enabled; });
            sync_directories_id = own("ck.files.sync_directories", [this] { show_sync_directories(); },
                                      [this] { return availability("ck.files.sync_directories").enabled; });
            diff_files_id = own("ck.files.diff_files", [this] { show_diff_files(); },
                                [this] { return availability("ck.files.diff_files").enabled; });
            archive_browse_id = own("ck.files.archive_browse", [this] { show_archive_browse(); },
                                    [this] { return availability("ck.files.archive_browse").enabled; });
            archive_create_id = own("ck.files.archive_create", [this] { show_archive_create(); },
                                    [this] { return availability("ck.files.archive_create").enabled; });
            archive_extract_id = own("ck.files.archive_extract", [this] { show_archive_extract(); },
                                     [this] { return availability("ck.files.archive_extract").enabled; });
            edit_id = own("ck.files.edit", [this] { show_embedded_editor(false); },
                          [this] { return availability("ck.files.edit").enabled; });
            new_edit_id = own("ck.files.new_edit", [this] { show_embedded_editor(true); },
                              [this] { return availability("ck.files.new_edit").enabled; });
            terminal_id = own("ck.files.terminal", [this] { show_embedded_terminal(); },
                              [this] { return availability("ck.files.terminal").enabled; });
            run_command_id = own("ck.files.run_command", [this] { show_run_command(); },
                                 [this] { return availability("ck.files.run_command").enabled; });
            user_menu_id = own("ck.files.user_menu", [this] { show_user_menu(); },
                               [this] { return availability("ck.files.user_menu").enabled; });
            connect_remote_id = own("ck.files.connect_remote", [this] { show_connect_remote(); },
                                    [this] { return availability("ck.files.connect_remote").enabled; });
            inspect_volume_archive_id = own("ck.files.inspect_volume_archive", [this] { show_inspect_volume_archive(); },
                                            [this] { return availability("ck.files.inspect_volume_archive").enabled; });
            restore_volume_archive_id = own("ck.files.restore_volume_archive", [this] { show_restore_volume_archive(); },
                                            [this] { return availability("ck.files.restore_volume_archive").enabled; });
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
            const auto confirm_focus = own(
                "ck.files.confirm_focus",
                [this] {
                    if (const auto target = panes[active].model.target())
                        begin_change(active, FilesPaneFocusRow{target->row});
                },
                [this] { return availability("ck.files.confirm_focus").enabled; });
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
                return ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{id, title});
            };
            SuiteShellOptions options;
            options.application_name = "Files";
            options.about_text =
                "Two-pane filesystem cockpit — F5 Copy · F6 Move · F3 Preview · F7 New folder · Ctrl+L Location.";
            options.theme = theme;
            options.application_menus = {
                ckv::widgets::MenuBarItem{
                    "&Location",
                    {item(location_id, "&Go to location..."), item(enter_id, "&Open selected entry"),
                     item(parent_id, "&Parent folder"), item(root_id, "Location &root"),
                     item(home_id, "&Home location"), item(history_back_id, "Go &back"),
                     item(history_forward_id, "Go &forward"), item(bookmark_open_id, "Open boo&kmark..."),
                     item(bookmark_add_id, "A&dd bookmark"),
                     item(bookmark_remove_id, "Remove bookmark"), item(refresh_id, "&Refresh listing"),
                     item(properties_id, "Propert&ies..."), item(preview_id, "Pre&view"),
                     item(copy_path_id, "Cop&y path"),
                     item(switch_id, "S&witch pane"), item(swap_panes_id, "S&wap panes"),
                     item(sync_other_pane_id, "Open in &other pane"), item(cancel_id, "&Cancel activity")}},
                ckv::widgets::MenuBarItem{"&Operations", {item(rename_id, "&Rename..."),
                                                            item(create_file_id, "New &file..."),
                                                            item(create_hard_link_id, "New &hard link..."),
                                                            item(create_symbolic_link_id, "New &symbolic link..."),
                                                            item(inspect_link_id, "Inspect link target"),
                                                            item(create_directory_id, "New f&older..."),
                                                            item(trash_id, "Move to &Trash / Recycle Bin..."),
                                                            item(delete_id, "&Delete permanently..."),
                                                            item(archive_recovery_id, "Inspect archive &recovery..."),
                                                            item(archive_cleanup_id, "Clean up archive staging..."),
                                                            item(operation_recovery_id, "Inspect operation recover&y..."),
                                                            item(operation_recovery_cleanup_id, "Clean up operation staging..."),
                                                            item(copy_id, "&Copy to other pane"),
                                                            item(move_id, "&Move to other pane")}},
                ckv::widgets::MenuBarItem{
                    "&Marks",
                    {item(toggle, "&Toggle mark"), item(mark_visible, "&Mark visible"),
                     item(unmark_visible, "&Unmark visible"), item(mark_pattern_id, "Mark by &pattern..."),
                     item(unmark_pattern_id, "Unmark by p&attern..."), item(compare_directories_id, "Co&mpare directories"),
                     item(invert, "&Invert visible marks"),
                     item(clear, "&Clear all marks (including hidden)"), item(confirm_focus, "Use current &row"),
                     item(range, "&Set range start"),
                     item(mark_range, "Mark th&rough cursor"), item(unmark_range, "Unmark through curs&or")}},
                ckv::widgets::MenuBarItem{
                    "&Tools",
                    {item(search_id, "Search &files..."), item(directory_tree_id, "Directory &tree / sizes..."),
                     item(diff_files_id, "&Diff selected files..."), item(sync_directories_id, "S&ynchronize directories..."),
                     item(archive_browse_id, "Browse &archive..."), item(archive_create_id, "Create archi&ve..."),
                     item(archive_extract_id, "E&xtract archive..."),
                     item(edit_id, "&Edit file..."), item(new_edit_id, "New &text document..."),
                     item(terminal_id, "Embedded &terminal"), item(run_command_id, "Run &command..."),
                     item(user_menu_id, "User &menu..."), item(connect_remote_id, "Connect &remote (SFTP/SMB)..."),
                     item(inspect_volume_archive_id, "Inspect &volume archive..."),
                     item(restore_volume_archive_id, "Restore volume arc&hive...")}},
                ckv::widgets::MenuBarItem{
                    "&View", {item(options_id, "View &options..."),
                              item(application.commands().standard().command_palette, "&Commands...")}}};
            options.application_status_items = {
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{enter_id, "&Open"}, 35},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{parent_id, "&Parent folder"}, 32},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{switch_id, "&Switch pane"}, 30},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{toggle, "&Mark"}, 28},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{cancel_id, "&Cancel activity"}, 25},
                ckv::widgets::StatusLineItem{
                    ckv::widgets::CommandPresentation{archive_recovery_id, "&Inspect archive recovery"}, 24},
                ckv::widgets::StatusLineItem{
                    ckv::widgets::CommandPresentation{archive_cleanup_id, "Clean up archive staging"}, 24},
                ckv::widgets::StatusLineItem{
                    ckv::widgets::CommandPresentation{operation_recovery_id, "Inspect operation recovery"}, 24},
                ckv::widgets::StatusLineItem{
                    ckv::widgets::CommandPresentation{operation_recovery_cleanup_id, "Clean up operation staging"}, 24},
                ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{options_id, "&View options"}, 20}};
            shell = std::make_unique<SuiteShell>(application, std::move(options));
            pane_menus = shell->menu_bar()->menus();
            const auto own_view_cmd = [&](std::string key, std::string title, std::string category,
                                          std::optional<ckv::KeyChord> chord, std::string chord_label,
                                          std::vector<std::string> contexts,
                                          std::function<void()> handler, std::function<bool()> enabled) {
                ckv::ui::CommandDescriptor desc;
                desc.key = std::move(key);
                desc.title = std::move(title);
                desc.category = std::move(category);
                desc.chord = std::move(chord_label);
                desc.scope = ckv::ui::CommandScope{.contexts = std::move(contexts)};
                desc.visibility = ckv::ui::CommandVisibility::Palette;
                const auto bound_contexts = desc.scope.contexts;
                desc.handler = std::move(handler);
                const auto id = commands.own(application.commands().declare(std::move(desc)));
                application.commands().set_enabled_predicate(id, std::move(enabled));
                if (chord) {
                    for (const auto &ctx : bound_contexts)
                        application.commands().bind_key(*chord, id, ctx);
                }
                return id;
            };
            const auto editor_active = [this] { return active_embedded_editor() != nullptr; };
            editor_save_id = own_view_cmd(
                "ck.files.editor.save", "&Save", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "s"}, "Ctrl+S", {"ck.files.editor"},
                [this] {
                    if (const auto idx = active_embedded_editor_index())
                        save_embedded_editor(*idx, false);
                },
                editor_active);
            editor_undo_id = own_view_cmd(
                "ck.files.editor.undo", "&Undo", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "z"}, "Ctrl+Z", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->perform(ckv::widgets::EditorCommand::Undo);
                },
                editor_active);
            editor_redo_id = own_view_cmd(
                "ck.files.editor.redo", "&Redo", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "y"}, "Ctrl+Y", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->perform(ckv::widgets::EditorCommand::Redo);
                },
                editor_active);
            editor_cut_id = own_view_cmd(
                "ck.files.editor.cut", "Cu&t", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "x"}, "Ctrl+X", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->cut_selection_to_clipboard();
                },
                editor_active);
            editor_copy_id = own_view_cmd(
                "ck.files.editor.copy", "&Copy", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "c"}, "Ctrl+C", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->copy_selection_to_clipboard();
                },
                editor_active);
            editor_paste_id = own_view_cmd(
                "ck.files.editor.paste", "&Paste", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "v"}, "Ctrl+V", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->paste_from_clipboard();
                },
                editor_active);
            editor_find_id = own_view_cmd(
                "ck.files.editor.find", "&Find", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "f"}, "Ctrl+F", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->perform(ckv::widgets::EditorCommand::FindSelection);
                },
                editor_active);
            editor_find_next_id = own_view_cmd(
                "ck.files.editor.find_next", "Find &next", "Editor",
                ckv::KeyChord{ckv::Key::F3, ckv::Modifier::None, {}}, "F3", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->find_next(true);
                },
                editor_active);
            editor_wrap_id = own_view_cmd(
                "ck.files.editor.wrap", "&Word wrap", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Alt, "w"}, "Alt+W", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->set_wrap_mode(ed->wrap_mode() == ckv::widgets::WrapMode::None
                                              ? ckv::widgets::WrapMode::Word
                                              : ckv::widgets::WrapMode::None);
                },
                editor_active);
            editor_line_numbers_id = own_view_cmd(
                "ck.files.editor.line_numbers", "&Line numbers", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Alt, "n"}, "Alt+N", {"ck.files.editor"},
                [this] {
                    if (auto *ed = active_embedded_editor())
                        ed->set_show_line_numbers(!ed->show_line_numbers());
                },
                editor_active);
            editor_close_id = own_view_cmd(
                "ck.files.editor.close", "&Close editor", "Editor",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "w"}, "Ctrl+W", {"ck.files.editor"},
                [this] {
                    if (const auto idx = active_embedded_editor_index())
                        if (embedded_editors[*idx].window && !embedded_editors[*idx].lifetime.expired())
                            embedded_editors[*idx].window->close();
                },
                editor_active);

            preview_show_hex_id = own_view_cmd(
                "ck.files.preview.show_hex", "Show &hex", "Preview",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "h"}, "H", {"ck.files.preview.text"},
                [this] {
                    if (preview_open() && preview_can_toggle && !preview_is_hex && preview_toggle_action)
                        preview_toggle_action();
                },
                [this] { return preview_open() && preview_can_toggle && !preview_is_hex; });
            preview_show_text_id = own_view_cmd(
                "ck.files.preview.show_text", "Show &text", "Preview",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::None, "t"}, "T", {"ck.files.preview.hex"},
                [this] {
                    if (preview_open() && preview_can_toggle && preview_is_hex && preview_toggle_action)
                        preview_toggle_action();
                },
                [this] { return preview_open() && preview_can_toggle && preview_is_hex; });
            preview_close_id = own_view_cmd(
                "ck.files.preview.close", "&Close preview", "Preview",
                ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, {}}, "Esc",
                {"ck.files.preview.text", "ck.files.preview.hex", "ck.files.preview.hex_only"},
                [this] {
                    if (preview_open())
                        preview_dialog_window->close();
                },
                [this] { return preview_open(); });

            terminal_focus_files_id = own_view_cmd(
                "ck.files.terminal.focus_files", "&Return to Files", "Terminal",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "o"}, "Ctrl+O", {"ck.files.terminal"},
                [this] {
                    if (surfaces[active] && surfaces[active]->grid)
                        application.set_focus(surfaces[active]->grid);
                    sync_active_view_chrome();
                },
                [this] { return embedded_terminal_window && !embedded_terminal_lifetime.expired(); });
            terminal_close_id = own_view_cmd(
                "ck.files.terminal.close", "&Close terminal", "Terminal",
                ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "w"}, "Ctrl+W", {"ck.files.terminal"},
                [this] {
                    if (embedded_terminal_window && !embedded_terminal_lifetime.expired())
                        embedded_terminal_window->close();
                },
                [this] { return embedded_terminal_window && !embedded_terminal_lifetime.expired(); });

            report_close_id = own_view_cmd(
                "ck.files.report.close", "&Close view", "View",
                ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, {}}, "Esc", {"ck.files.report"},
                [this] {
                    if (parity_report_window && !parity_report_lifetime.expired())
                        parity_report_window->close();
                },
                [this] { return parity_report_window && !parity_report_lifetime.expired(); });

            if (auto *status = shell->status_line()) {
                status->set_context_items("ck.files.editor", {
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{editor_save_id, "&Save", "Ctrl+S"}, 40},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{editor_close_id, "&Close", "Ctrl+W"}, 38},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{editor_undo_id, "&Undo", "Ctrl+Z"}, 35},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{editor_redo_id, "&Redo", "Ctrl+Y"}, 32},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{editor_find_id, "&Find", "Ctrl+F"}, 30},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{editor_wrap_id, "&Wrap", "Alt+W"}, 25},
                });
                status->set_context_items("ck.files.preview.text", {
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{preview_show_hex_id, "Show &hex", "H"}, 40},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{preview_close_id, "&Close", "Esc"}, 38},
                });
                status->set_context_items("ck.files.preview.hex", {
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{preview_show_text_id, "Show &text", "T"}, 40},
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{preview_close_id, "&Close", "Esc"}, 38},
                });
                status->set_context_items("ck.files.preview.hex_only", {
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{preview_close_id, "&Close", "Esc"}, 38},
                });
                status->set_context_items("ck.files.terminal", {
                    ckv::widgets::StatusLineItem{
                        ckv::widgets::CommandPresentation{terminal_focus_files_id, "&Return to Files", "Ctrl+O"}, 40},
                    ckv::widgets::StatusLineItem{
                        ckv::widgets::CommandPresentation{terminal_close_id, "&Close terminal", "Ctrl+W"}, 35},
                });
                status->set_context_items("ck.files.report", {
                    ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{report_close_id, "&Close view", "Esc"}, 40},
                });
            }

            shell->menu_bar()->on_highlight_changed = [this](const ckv::widgets::MenuHighlight &highlight) {
                if (closing)
                    return;
                highlighted_menu_command = highlight.command;
                labels();
            };
            create_window();
            last_input_mouse_count = application.mouse_events_dispatched();
            observer = application.add_attention_observer([this](auto kind) {
                if (closing)
                    return;
                if (kind == ckv::ui::Application::AttentionChange::Focus) {
                    for (std::size_t i = 0; i < 2; ++i)
                        if (surfaces[i] && contains(surfaces[i], application.focused()) && active != i) {
                            active = i;
                            layout();
                        }
                    sync_active_view_chrome();
                    const bool focused = dynamic_cast<ckv::widgets::CommandPalette *>(application.focused()) != nullptr;
                    if (palette_focused != focused) {
                        palette_focused = focused;
                        wake.request();
                    }
                } else {
                    update_footer_hint(kind);
                    if (palette_focused && kind == ckv::ui::Application::AttentionChange::Input)
                        wake.request(); // Refresh after the palette has handled the input.
                }
            });
            wake.bind([this] { pump(); });
            bound = true;
            if (volume_archive_recovery_service)
                scan_archive_recovery();
            if (operation_recovery_service)
                scan_operation_recovery();
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
    ckv::ui::CommandId footer_command_at(const ckv::widgets::StatusLine &footer, ckv::Point cell) const {
        constexpr int item_padding = 1;
        const ckv::Rect abs = footer.absolute_bounds();
        if (!abs.contains(cell) || cell.y != abs.y)
            return ckv::ui::kInvalidCommand;
        const auto &items = footer.shown_items();
        if (items.empty())
            return ckv::ui::kInvalidCommand;
        const auto item_width = [this](const ckv::widgets::StatusLineItem &item) {
            const ckv::ui::CommandId command = item.presentation.command;
            if (command == ckv::ui::kInvalidCommand)
                return ckv::text::text_width(item.label);
            const ckv::ui::CommandInfo *info = application.commands().find(command);
            if (!info)
                return ckv::text::text_width(item.presentation.label.empty() ? item.label : item.presentation.label);
            const std::string text = ckv::widgets::parse_mnemonic(
                item.presentation.label.empty() ? info->title : item.presentation.label).display;
            if (!item.presentation.chord.empty())
                return ckv::text::text_width(item.presentation.chord + " " + text);
            const std::string shortcut = application.commands().chord_text(command);
            return ckv::text::text_width(shortcut.empty() ? text : shortcut + " " + text);
        };
        const auto separator_width = [&](std::size_t before, std::size_t after) {
            if (footer.presentation() == ckv::widgets::StatusLinePresentation::Grouped) {
                for (std::size_t idx = before + 1; idx <= after; ++idx)
                    if (items[idx].group_break_before)
                        return 3;
            }
            return 2;
        };
        std::vector<int> widths(items.size());
        std::vector<std::size_t> visible(items.size());
        for (std::size_t idx = 0; idx < items.size(); ++idx) {
            widths[idx] = item_width(items[idx]);
            visible[idx] = idx;
        }
        const int available = std::max(0, abs.width - item_padding);
        const auto total_width = [&] {
            int width = 0;
            for (std::size_t pos = 0; pos < visible.size(); ++pos) {
                if (pos > 0)
                    width += separator_width(visible[pos - 1], visible[pos]);
                width += widths[visible[pos]];
            }
            return width;
        };
        const bool equal_priorities = std::all_of(
            items.begin(), items.end(),
            [priority = items.front().priority](const ckv::widgets::StatusLineItem &item) {
                return item.priority == priority;
            });
        if (equal_priorities) {
            int occupied = 0;
            std::size_t count = 0;
            for (const std::size_t idx : visible) {
                const int start = occupied + (count == 0 ? 0 : separator_width(visible[count - 1], idx));
                if (start >= available)
                    break;
                ++count;
                occupied = start + widths[idx];
            }
            visible.resize(count);
        } else {
            while (!visible.empty() && total_width() > available) {
                auto remove = visible.begin();
                for (auto it = visible.begin(); it != visible.end(); ++it) {
                    if (items[*it].priority < items[*remove].priority ||
                        (items[*it].priority == items[*remove].priority && it > remove))
                        remove = it;
                }
                visible.erase(remove);
            }
        }
        const int local_x = cell.x - abs.x;
        int x = item_padding;
        for (std::size_t pos = 0; pos < visible.size(); ++pos) {
            if (pos > 0)
                x += separator_width(visible[pos - 1], visible[pos]);
            const std::size_t idx = visible[pos];
            if (local_x >= x - item_padding && local_x < x + widths[idx] + item_padding)
                return items[idx].presentation.command;
            x += widths[idx];
        }
        return ckv::ui::kInvalidCommand;
    }
    void update_footer_hint(ckv::ui::Application::AttentionChange kind) {
        auto next = ckv::ui::kInvalidCommand;
        const auto *footer = shell->status_line();
        if (kind == ckv::ui::Application::AttentionChange::Input) {
            const auto count = application.mouse_events_dispatched();
            const auto &mouse = application.last_mouse_event();
            if (count != last_input_mouse_count && mouse && mouse->action == ckv::MouseAction::Down &&
                mouse->button == ckv::MouseButton::Left && footer && application.hovered_view() == footer &&
                !application.input_capture()) {
                const auto hit = footer_command_at(*footer, mouse->cell);
                if (hit != ckv::ui::kInvalidCommand && !application.commands().is_enabled(hit))
                    next = hit;
            }
            last_input_mouse_count = count;
        } else if (application.hovered_view() == footer) {
            return;
        }
        if (next != pressed_footer_command) {
            pressed_footer_command = next;
            labels();
        }
    }
    void cleanup() noexcept {
        if (settings.worker)
            settings.worker->request_close();
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
        bookmark_dialog.reset();
        bookmark_dialog_window = nullptr;
        link_dialog.reset();
        link_dialog_window = nullptr;
        create_directory_dialog.reset();
        create_directory_window = nullptr;
        create_file_dialog.reset();
        create_file_window = nullptr;
        create_hard_link_dialog.reset();
        create_hard_link_window = nullptr;
        create_symbolic_link_dialog.reset();
        create_symbolic_link_window = nullptr;
        rename_dialog.reset();
        rename_window = nullptr;
        trash_dialog.reset();
        trash_window = nullptr;
        delete_dialog.reset();
        delete_window = nullptr;
        archive_recovery_window = nullptr;
        archive_cleanup_dialog.reset();
        archive_cleanup_window = nullptr;
        volume_archive_recovery_cleanup_request = false;
        volume_archive_recovery_cleanup_error.clear();
        operation_recovery_dialog.reset();
        operation_recovery_window = nullptr;
        reconciliation_overlap_window = nullptr;
        volume_request.reset();
        operation_review_dialog.reset();
        tree_copy_dialog.reset();
        tree_copy_window = nullptr;
        operation_planning_request.reset();
        operation_execution_request.reset();
        operation_execution_origin.reset();
        operation_planning_encoding.reset();
        operation_planning_revalidation = {};
        operation_planning_completion = {};
        operation_planning_transfer.reset();
        volume_archive_review_dialog.reset();
        volume_archive_planning_request.reset();
        volume_archive_execution_request.reset();
        volume_archive_origin.reset();
        for (auto *surface : surfaces)
            if (surface) {
                surface->grid->clear_model();
                surface->grid->on_change = {};
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
        if (volume_archive_service)
            volume_archive_service->request_stop();
        if (volume_archive_recovery_service)
            volume_archive_recovery_service->request_stop();
        if (volume_archive_recovery_cleanup_service)
            volume_archive_recovery_cleanup_service->request_stop();
        if (operation_recovery_service)
            operation_recovery_service->request_stop();
        if (operation_recovery_cleanup_service)
            operation_recovery_cleanup_service->request_stop();
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
                if (active == i && panes[i].model.cursor_kind() == ck::files::ObjectKind::Directory)
                    application.execute_command(enter_id);
            };
            surface->grid->on_change = [this] {
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
        return confirmation.has_value() || (view_dialog && !view_dialog->completed()) ||
               (bookmark_dialog && !bookmark_dialog->completed()) ||
               properties_open() ||
               preview_open() ||
               (link_dialog && !link_dialog->completed()) ||
               (create_directory_dialog && !create_directory_dialog->completed()) ||
               (create_file_dialog && !create_file_dialog->completed()) ||
               (create_hard_link_dialog && !create_hard_link_dialog->completed()) ||
               (create_symbolic_link_dialog && !create_symbolic_link_dialog->completed()) ||
               (rename_dialog && !rename_dialog->completed()) ||
               (trash_dialog && !trash_dialog->completed()) ||
               (delete_dialog && !delete_dialog->completed()) ||
               (mark_pattern_dialog && !mark_pattern_dialog->completed()) ||
               (parity_dialog && !parity_dialog->completed()) ||
               archive_recovery_open() ||
               reconciliation_overlap_open() ||
               (archive_cleanup_dialog && !archive_cleanup_dialog->completed()) ||
               (operation_recovery_dialog && !operation_recovery_dialog->completed()) ||
               (operation_recovery_cleanup_dialog && !operation_recovery_cleanup_dialog->completed()) ||
               (volume_archive_review_dialog && !volume_archive_review_dialog->completed()) ||
               volume_archive_planning_request.has_value() ||
               (operation_review_dialog && !operation_review_dialog->completed()) ||
               (tree_copy_dialog && !tree_copy_dialog->completed()) ||
               (location_dialog && !location_dialog->completed()) || operation_planning_request.has_value();
    }
    const FilesVolumeArchiveRecoveryItem *archive_cleanup_item() const noexcept {
#if !defined(_WIN32)
        return nullptr;
#else
        if (!volume_archive_recovery_scan || volume_archive_recovery_scan->journals.size() != 1)
            return nullptr;
        const auto &item = volume_archive_recovery_scan->journals.front();
        if (!item.native_name || !item.report || item.report->operation.items.size() != 1)
            return nullptr;
        const auto &recovery = item.report->operation.items.front();
        if (recovery.action != ck::files::OperationRecoveryAction::InspectUnpublishedStaging ||
            (recovery.state != ck::files::OperationJournalState::Copied &&
             recovery.state != ck::files::OperationJournalState::Verified))
            return nullptr;
        const auto &evidence = item.report->evidence;
        if (!evidence.destination_encoding || evidence.destination_native_path.empty() ||
            !evidence.staging_encoding || evidence.staging_native_path.empty() || !evidence.staging_digest)
            return nullptr;
        return &item;
#endif
    }
    const FilesOperationRecoveryItem *operation_cleanup_item() const noexcept {
#if !defined(_WIN32)
        return nullptr;
#else
        if (!operation_recovery_scan || operation_recovery_scan->journals.size() != 1)
            return nullptr;
        const auto &item = operation_recovery_scan->journals.front();
        if (item.kind != FilesOperationJournalKind::Transfer || !item.native_name || !item.report)
            return nullptr;
        const auto &inspection = item.report->inspection;
        if (inspection.report.items.size() != 1 || inspection.artifacts.size() != 1 ||
            inspection.report.has_untrusted_tail || inspection.has_untrusted_evidence)
            return nullptr;
        const auto &recovery = inspection.report.items.front();
        const auto &artifact = inspection.artifacts.front();
        if (recovery.action != ck::files::OperationRecoveryAction::InspectUnpublishedStaging ||
            (recovery.state != ck::files::OperationJournalState::Copied &&
             recovery.state != ck::files::OperationJournalState::Verified) ||
            !artifact.evidence || !artifact.evidence->staging_identity ||
            !artifact.evidence->destination_address)
            return nullptr;
        return &item;
#endif
    }
    FilesCommandAvailability availability(std::string_view command) const {
        const auto unavailable = [](std::string reason) {
            return FilesCommandAvailability::unavailable(std::move(reason));
        };
        if (closing)
            return unavailable(std::string(command) + " unavailable: Files is closing");
        if (panes[active].selection.invalidated_refresh_focus &&
            (command == "ck.files.rename" || command == "ck.files.trash" ||
             command == "ck.files.permanently_delete" || command == "ck.files.create_hard_link"))
            return unavailable("Use current row before acting on this entry");
        if (command == "ck.files.switch_pane")
            return modal_pending() ? unavailable("Switch pane unavailable: a dialog is open")
                                   : FilesCommandAvailability::available();
        if (command == "ck.files.cancel_read") {
            if (operation_planning_request && planning_service)
                return operation_planning_cancelling ? unavailable("Cancellation already requested")
                                                     : FilesCommandAvailability::available();
            if (volume_archive_planning_request && volume_archive_service)
                return volume_archive_planning_cancelling ? unavailable("Cancellation already requested")
                                                          : FilesCommandAvailability::available();
            if (modal_pending())
                return unavailable("Cancel unavailable: a dialog is open");
            if (volume_archive_execution_request && volume_archive_service) {
                const auto progress = volume_archive_service->progress(*volume_archive_execution_request);
                return progress && progress->phase == FilesVolumeArchivePhase::CancelRequested
                    ? unavailable("Cancellation already requested") : FilesCommandAvailability::available();
            }
            if (operation_execution_request ||
                pending(panes[active]) || volume_pending(active) || panes[active].volume_view)
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
        if (command == "ck.files.inspect_operation_recovery") {
            if (operation_execution_request)
                return unavailable("Recovery inspection unavailable: a file operation is writing its journal");
            if (operation_recovery_request)
                return unavailable("Recovery inspection unavailable: operation recovery scan is still running");
            if (!operation_recovery_scan)
                return unavailable(operation_recovery_error.empty()
                                       ? "Recovery inspection unavailable: operation recovery scan is not ready"
                                       : "Recovery inspection unavailable: " + operation_recovery_error);
            return operation_recovery_scan->journals.empty()
                       ? unavailable("Recovery inspection unavailable: no operation journals were found")
                       : FilesCommandAvailability::available();
        }
        if (command == "ck.files.cleanup_archive_recovery") {
            if (!volume_archive_recovery_cleanup_service)
                return unavailable("Archive staging cleanup unavailable: cleanup worker is not configured");
            if (volume_archive_recovery_cleanup_request)
                return unavailable("Archive staging cleanup unavailable: cleanup is still running");
            if (modal_pending())
                return unavailable("Archive staging cleanup unavailable: a dialog is open");
            if (volume_archive_recovery_request)
                return unavailable("Archive staging cleanup unavailable: recovery scan is still running");
            if (!volume_archive_recovery_scan)
                return unavailable("Archive staging cleanup unavailable: recovery scan is not ready");
            return unavailable("Archive staging cleanup unavailable: live archive ownership is required");
        }
        if (command == "ck.files.cleanup_operation_recovery") {
            if (!operation_recovery_cleanup_service)
                return unavailable("Operation staging cleanup unavailable: cleanup worker is not configured");
            if (operation_recovery_cleanup_request)
                return unavailable("Operation staging cleanup unavailable: cleanup is still running");
            if (modal_pending())
                return unavailable("Operation staging cleanup unavailable: a dialog is open");
            if (operation_recovery_request)
                return unavailable("Operation staging cleanup unavailable: recovery scan is still running");
            if (!operation_recovery_scan)
                return unavailable("Operation staging cleanup unavailable: recovery scan is not ready");
            return unavailable("Operation staging cleanup unavailable: live copy ownership is required");
        }
        if (command == "ck.files.parent") {
            if (panes[active].volume_view) {
                if (panes[active].navigating || modal_pending())
                    return unavailable("Parent folder unavailable: active pane is busy");
                return services[active]->running() ? FilesCommandAvailability::available()
                                                   : unavailable("Parent folder unavailable: location worker stopped");
            }
            if (!ready(active))
                return unavailable("Parent folder unavailable: active pane is not ready");
            if (!panes[active].active || !panes[active].active->parent().value())
                return unavailable("Parent folder unavailable: location has no parent");
            if (panes[active].active->components().empty()) {
                if (volume_request)
                    return unavailable("Volumes unavailable: volume information is still loading");
                if (!volume_service || !volume_service->running())
                    return unavailable("Volumes unavailable: volume information worker is unavailable");
            }
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
        if (command == "ck.files.bookmark_open") {
            if (!ready(active))
                return unavailable("Open bookmark unavailable: active pane is not ready");
            return panes[active].bookmarks.empty()
                       ? unavailable("Open bookmark unavailable: this pane has no bookmarks")
                       : FilesCommandAvailability::available();
        }
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
            return refresh_availability(active);
        if (command == "ck.files.properties") {
            const auto &pane = panes[active];
            if (pane.volume_view) {
                if (modal_pending() || pending(pane) || volume_pending(active))
                    return unavailable("Properties unavailable: volume information is busy");
                return pane.model.volume_cursor() ? FilesCommandAvailability::available()
                    : unavailable("Properties unavailable: select a volume");
            }
            return ready(active) && panes[active].model.cursor_row()
                       ? FilesCommandAvailability::available()
                       : unavailable("Properties unavailable: no current entry is selected");
        }
        if (command == "ck.files.copy_path") {
            if (std::any_of(panes.begin(), panes.end(), [](const Pane &pane) {
                    return pane.inspection && pane.inspection->kind == FilesPaneReplyKind::PathText;
                }))
                return unavailable("Copy path unavailable: a path is already being read");
            return ready(active) && panes[active].model.target()
                       ? FilesCommandAvailability::available()
                       : unavailable("Copy path unavailable: select a current file or folder entry");
        }
        if (command == "ck.files.preview")
            return ready(active) && panes[active].model.cursor_kind() == ck::files::ObjectKind::RegularFile
                       ? FilesCommandAvailability::available()
                       : unavailable("Preview unavailable: select a regular file");
        if (command == "ck.files.inspect_link") {
            if (panes[active].volume_view)
                return unavailable("Link inspection unavailable: volume rows are archive sources, not entries");
            const auto kind = panes[active].model.cursor_kind();
            const bool link = kind == ck::files::ObjectKind::SymbolicLink ||
                              kind == ck::files::ObjectKind::Junction ||
                              kind == ck::files::ObjectKind::ReparsePoint;
            return ready(active) && link
                       ? FilesCommandAvailability::available()
                       : unavailable("Link inspection unavailable: select a symbolic link or reparse point");
        }
        if (command == "ck.files.enter_directory") {
            if (panes[active].volume_view)
                return unavailable("Open unavailable: return to the filesystem listing");
            const auto kind = panes[active].model.cursor_kind();
            return ready(active) && (kind == ck::files::ObjectKind::Directory || kind == ck::files::ObjectKind::RegularFile)
                       ? FilesCommandAvailability::available()
                       : unavailable("Open unavailable: select a file or folder");
        }
        if (command == "ck.files.create_directory" || command == "ck.files.create_file") {
            const auto *label = command == "ck.files.create_directory" ? "New folder" : "New file";
            if (!ready(active))
                return unavailable(std::string(label) + " unavailable: active pane is not ready");
            return panes[active].read_only_mount.value_or(false)
                ? unavailable(std::string(label) + " unavailable: active mount is read-only")
                : FilesCommandAvailability::available();
        }
        if (command == "ck.files.create_hard_link") {
            if (panes[active].volume_view)
                return unavailable("Hard link unavailable: volume rows are archive sources, not entries");
            if (!ready(active) || panes[active].model.cursor_kind() != ck::files::ObjectKind::RegularFile)
                return unavailable("Hard link unavailable: select a regular file");
            return panes[active].read_only_mount.value_or(false)
                ? unavailable("Hard link unavailable: active mount is read-only")
                : FilesCommandAvailability::available();
        }
        if (command == "ck.files.create_symbolic_link") {
            if (panes[active].volume_view)
                return unavailable("Symbolic link unavailable: volume rows are archive sources, not entries");
#if defined(_WIN32)
            return unavailable("Symbolic link unavailable: handle-relative Windows reparse publication is not qualified");
#else
            if (!ready(active))
                return unavailable("Symbolic link unavailable: active pane is not ready");
            return panes[active].read_only_mount.value_or(false)
                ? unavailable("Symbolic link unavailable: active mount is read-only")
                : FilesCommandAvailability::available();
#endif
        }
        if (command == "ck.files.rename" || command == "ck.files.trash" ||
            command == "ck.files.permanently_delete") {
            const auto *label = command == "ck.files.rename" ? "Rename"
                : command == "ck.files.trash" ? "Trash / Recycle Bin" : "Delete";
            if (panes[active].volume_view)
                return unavailable(std::string(label) + " unavailable: volume rows are archive sources, not entries");
            if (!ready(active) || !panes[active].model.target())
                return unavailable("Operation unavailable: select a current entry");
            if (panes[active].read_only_mount.value_or(false))
                return unavailable(std::string(label) + " unavailable: active mount is read-only");
            if (command == "ck.files.permanently_delete") {
                if (panes[active].model.cursor_kind() != ck::files::ObjectKind::Directory)
                    return FilesCommandAvailability::available();
                if (!execution_service || !operation_journal_root || operation_execution_request)
                    return unavailable("Delete unavailable: recursive deletion worker is not ready");
            }
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.swap_panes") {
            if (modal_pending())
                return unavailable("Swap panes unavailable: a dialog is open");
            if (panes[0].volume_view || panes[1].volume_view || !panes[0].active || !panes[1].active)
                return unavailable("Swap panes unavailable: both panes need active locations");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.sync_other_pane") {
            if (modal_pending())
                return unavailable("Open in other pane unavailable: a dialog is open");
            if (panes[active].volume_view || !panes[active].active)
                return unavailable("Open in other pane unavailable: active pane has no folder location");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.compare_directories") {
            if (modal_pending())
                return unavailable("Compare directories unavailable: a dialog is open");
            if (!ready(0) || !ready(1) || panes[0].volume_view || panes[1].volume_view)
                return unavailable("Compare directories unavailable: both panes must be ready");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.mark_pattern" || command == "ck.files.unmark_pattern") {
            if (modal_pending())
                return unavailable("Pattern selection unavailable: a dialog is open");
            if (!ready(active) || panes[active].volume_view)
                return unavailable("Pattern selection unavailable: active pane is not ready");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.search" || command == "ck.files.directory_tree" ||
            command == "ck.files.terminal" || command == "ck.files.run_command" ||
            command == "ck.files.user_menu" || command == "ck.files.new_edit" ||
            command == "ck.files.connect_remote") {
            if (modal_pending())
                return unavailable("Command unavailable: a dialog is open");
            if (command != "ck.files.connect_remote" &&
                (!ready(active) || panes[active].volume_view || !panes[active].active))
                return unavailable("Command unavailable: active pane has no folder location");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.sync_directories") {
            if (modal_pending())
                return unavailable("Synchronize unavailable: a dialog is open");
            if (!ready(0) || !ready(1) || panes[0].volume_view || panes[1].volume_view ||
                !panes[0].active || !panes[1].active)
                return unavailable("Synchronize unavailable: both panes need active folder locations");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.diff_files" || command == "ck.files.edit" ||
            command == "ck.files.archive_browse" || command == "ck.files.archive_extract" ||
            command == "ck.files.inspect_volume_archive" || command == "ck.files.restore_volume_archive") {
            if (modal_pending())
                return unavailable("Command unavailable: a dialog is open");
            if (!ready(active) || panes[active].volume_view || !panes[active].active)
                return unavailable("Command unavailable: active pane is not ready");
            if (panes[active].model.cursor_kind() != ck::files::ObjectKind::RegularFile)
                return unavailable("Command unavailable: select a regular file");
            if ((command == "ck.files.archive_extract" || command == "ck.files.restore_volume_archive") &&
                (!ready(1 - active) || panes[1 - active].volume_view || !panes[1 - active].active))
                return unavailable("Command unavailable: opposite pane needs a destination folder");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.archive_create") {
            if (modal_pending())
                return unavailable("Create archive unavailable: a dialog is open");
            if (!ready(active) || !ready(1 - active) || panes[active].volume_view ||
                panes[1 - active].volume_view || !panes[active].active || !panes[1 - active].active)
                return unavailable("Create archive unavailable: both panes need active folders");
            if (!panes[active].model.target())
                return unavailable("Create archive unavailable: select an entry to archive");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.resume_operation_recovery" ||
            command == "ck.files.retry_operation_recovery" ||
            command == "ck.files.restore_retained_target" ||
            command == "ck.files.dispose_retained_target") {
            if (!operation_recovery_service || !operation_journal_root)
                return unavailable("Operation recovery execution unavailable: service not configured");
            if (operation_recovery_request || operation_recovery_action_pending)
                return unavailable("Operation recovery is busy");
            if (!operation_recovery_scan || operation_recovery_scan->journals.empty())
                return unavailable("No recorded operation recovery journals");
            const bool want_retention = command == "ck.files.restore_retained_target" ||
                                        command == "ck.files.dispose_retained_target";
            const bool found = std::any_of(operation_recovery_scan->journals.begin(),
                                           operation_recovery_scan->journals.end(),
                                           [&](const FilesOperationRecoveryItem &item) {
                                               return item.native_name.has_value() && item.report.has_value() &&
                                                      (item.kind == FilesOperationJournalKind::RetainedTarget) == want_retention;
                                           });
            if (!found)
                return unavailable(want_retention ? "No retained target versions available"
                                                  : "No unfinished transfer journals available");
            return FilesCommandAvailability::available();
        }
        if (command == "ck.files.copy" || command == "ck.files.move") {
            const bool move = command == "ck.files.move";
            if (operation_planning_request || operation_execution_request ||
                volume_archive_execution_request || volume_archive_planning_request ||
                (volume_archive_review_dialog && !volume_archive_review_dialog->completed()))
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: another operation is running");
            if (panes[active].volume_view) {
                if (move)
                    return unavailable("Move unavailable: volume rows are archive sources, not movable entries");
                if (panes[active].navigating)
                    return unavailable("Copy unavailable: active pane is opening a location");
                if (volume_pending(active))
                    return unavailable("Copy unavailable: volume information is still loading");
                if (panes[active].volume_inventory_stale)
                    return unavailable("Copy unavailable: refresh the volume information first");
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
                if (!volume_archive_service || !volume_archive_service->running())
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
                if (!ck::files::NativeArchiveWriteJob::platform_supports_publication())
                    return unavailable("Copy: archive writes unsupported");
                if (!volume_archive_scratch_root || !volume_archive_scratch_root->is_valid())
                    return unavailable("Copy unavailable: private archive capture storage is not configured");
                if (!volume->archive.root)
                    return unavailable("Copy unavailable: source volume mount identity is unavailable");
                if (!archive_destination_ready(active))
                    return unavailable("Copy unavailable: destination pane is not ready");
                if (panes[1 - active].read_only_mount.value_or(false))
                    return unavailable("Copy unavailable: destination mount is read-only");
                return FilesCommandAvailability::available();
            }
            if (!planning_service || !execution_service || !operation_journal_root)
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: operation worker is not configured");
            if (!planning_service->running() || !execution_service->running())
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: operation worker stopped");
            if (!ready(active) || !ready(1 - active))
                return unavailable(std::string(move ? "Move" : "Copy") +
                                   " unavailable: both panes must be ready");
            if (panes[1 - active].read_only_mount.value_or(false))
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: destination mount is read-only");
            if (move && panes[active].read_only_mount.value_or(false))
                return unavailable("Move unavailable: source mount is read-only");
            if (services[active]->working() || services[1 - active]->working())
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: a pane worker is busy");
            const auto &selection = panes[active].selection;
            if (selection.invalidated_refresh_marks)
                return unavailable(std::string(move ? "Move" : "Copy") +
                                   " unavailable: change or clear marks after refresh");
            if (selection.invalidated_refresh_focus && (!selection.marks || !selection.marks->total))
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: choose Use current row");
            if (selection.state == ck::files::PaneSourcesState::HiddenMarksNeedReview)
                return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: reveal or clear hidden marks");
            if (selection.origin == ck::files::PaneSourcesOrigin::Marks) {
                if (selection.state != ck::files::PaneSourcesState::Ready || !selection.candidates)
                    return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: marked entries are not ready");
                if (selection.candidates > ck::files::IndexedSourceLimits{}.held_entries)
                    return unavailable(std::string(move ? "Move" : "Copy") + " unavailable: too many entries are marked");
                return FilesCommandAvailability::available();
            }
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
            return ready(active) && ((panes[active].selection.marks && panes[active].selection.marks->total != 0) ||
                                     panes[active].selection.invalidated_refresh_marks)
                       ? FilesCommandAvailability::available()
                       : unavailable("Clear marks unavailable: no entries are marked");
        if (command == "ck.files.confirm_focus")
            return ready(active) && panes[active].selection.invalidated_refresh_focus && panes[active].model.target()
                       ? FilesCommandAvailability::available()
                       : unavailable("Use current row unavailable: no unresolved current row");
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
        return p.navigating || p.reading || p.changing || p.queued_view || p.queued_refresh ||
               p.queued_copy.has_value() || !p.queued_marks.empty() ||
               !p.batch_delete_names.empty() || p.inspection || p.mutating;
    }
    bool volume_pending(std::size_t i) const {
        return volume_request && volume_request->pane == i && !volume_request->retired &&
               volume_request->navigation == panes[i].navigation;
    }
    bool ready(std::size_t i, bool continuation = false, bool allow_stale_listing = false) const {
        return i < 2 && !closing && !operation_execution_request && (continuation || !modal_pending()) &&
               !pending(panes[i]) && !volume_pending(i) && !panes[i].volume_view && panes[i].active &&
               panes[i].model.current() && (allow_stale_listing || !panes[i].refresh_failed) && services[i]->running();
    }
    bool archive_destination_ready(std::size_t source_pane) const {
        if (source_pane >= 2 || !volume_archive_service || !volume_archive_service->running())
            return false;
        const auto destination_pane = 1 - source_pane;
        const auto &destination = panes[destination_pane];
        return !destination.volume_view && !destination.refresh_failed && !pending(destination) &&
               !volume_pending(destination_pane) && destination.active &&
               destination.model.current() && services[destination_pane]->running();
    }
    std::string volume_archive_status() const {
        const auto progress = volume_archive_service->progress(*volume_archive_execution_request);
        if (!progress)
            return "Preparing archive...";
        using Phase = FilesVolumeArchivePhase;
        switch (progress->phase) {
            case Phase::Writing:
            case Phase::Verifying:
                return std::string(progress->phase == Phase::Writing ? "Writing archive " : "Verifying archive ") +
                    volume_bytes(progress->completed_bytes) + " / " + volume_bytes(progress->total_bytes);
            case Phase::Publishing:
                return "Publishing archive...";
            case Phase::Closing:
                return "Closing archive...";
            case Phase::CancelRequested:
                return "Archive cancellation requested...";
            case Phase::Preparing:
                return "Preparing archive...";
        }
        return "Creating volume archive...";
    }
    std::string status(std::size_t i) const {
        const auto &p = panes.at(i);
        std::string text;
        if (volume_archive_planning_request)
            text = volume_archive_planning_cancelling ? "Cancelling archive planning..." : "Planning volume archive...";
        else if (volume_archive_execution_request)
            text = volume_archive_status();
        else if (operation_planning_request) {
            if (operation_planning_cancelling)
                text = "Cancelling operation planning...";
            else if (operation_planning_transfer)
                text = operation_planning_transfer->second.operation == ck::files::FileOperation::Move
                    ? "Planning move... Esc cancels" : "Planning copy... Esc cancels";
            else
                text = "Planning operation... Esc cancels";
        } else if (operation_execution_request)
            text = operation_execution_origin->kind == ck::files::FileOperation::Move
                ? "Moving to the other pane..."
                : operation_execution_origin->kind == ck::files::FileOperation::Delete
                    ? "Deleting selected tree..." : "Copying to the other pane...";
        else if (volume_pending(i))
            text = "Reading volume information...";
        else if (p.cancelling)
            text = "Cancelling activity...";
        else if (p.navigating)
            text = p.refreshing ? "Refreshing listing..." : p.entering ? "Opening folder..."
                              : p.homing ? "Opening home location..." : "Opening location...";
        else if (p.volume_view) {
            const auto volume = p.model.volume_cursor();
            if (!p.error.empty())
                text = p.navigation_error ? "Volume view kept. " + p.error : p.error;
            else if (volume)
                text = (!volume->read_only ? "Access unknown" : *volume->read_only ? "RO" : "RW") +
                       std::string(" | ") + volume->display_mount() + " | " +
                       (volume->filesystem.empty() ? "Unknown" : ck::files::display_native_text(volume->filesystem)) + " | " +
                       volume_bytes(volume->free_bytes) + " free / " + volume_bytes(volume->total_bytes);
            else
                text = "No visible volumes";
        } else if (p.preparing_cursor)
            text = p.queued_refresh ? "Reading current row for refresh..."
                 : p.queued_copy    ? (*p.queued_copy == ck::files::FileOperation::Move
                                           ? "Reading current row for Move..."
                                           : "Reading current row for Copy...")
                                    : "Reading current row for view change...";
        else if (p.inspection)
            text = p.inspection->kind == FilesPaneReplyKind::PathText ? "Reading selected path..."
                   : p.inspection->kind == FilesPaneReplyKind::Link  ? "Reading link metadata..."
                                                                     : "Reading preview...";
        else if (p.mutating)
            text = p.renaming ? "Renaming..." : p.trashing ? "Moving to Trash / Recycle Bin..."
                              : p.deleting ? "Deleting permanently..." : "Creating folder...";
        else if (p.changing)
            text = *p.changing == FilesPaneReplyKind::Focus   ? (p.queued_refresh ? "Preparing current row for refresh..."
                                                                 : p.queued_copy  ? (*p.queued_copy == ck::files::FileOperation::Move
                                                                                         ? "Preparing current row for Move..."
                                                                                         : "Preparing current row for Copy...")
                                                                                  : "Preparing current row for view change...")
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
        else if (!p.archive_notice.empty())
            text = p.archive_notice;
        else {
            if (p.selection.invalidated_refresh_focus)
                text = "Choose Use current row";
            if (p.selection.invalidated_refresh_marks) {
                if (!text.empty())
                    text += " | ";
                text += "Marks lost on refresh: " + std::to_string(p.selection.invalidated_refresh_marks);
            }
            if (text.empty())
                text = "Last scan: " + std::to_string(p.model.row_count()) + " items";
        }
        if (p.active && (!p.model.current() || p.refresh_failed) && text.find("Stale") == text.npos)
            text += "; stale listing";
        if (p.volume_view && p.volume_inventory_stale)
            text += "; previous volume information retained";
        if (p.navigation_error && p.active && !p.volume_view)
            text += "; previous location retained";
        if (p.selection.marks && p.selection.marks->total) {
            text += " | Marked: " + std::to_string(p.selection.marks->total);
            if (p.selection.marks->hidden)
                text += " (" + std::to_string(p.selection.marks->hidden) + " hidden; review required)";
        }
        if (p.range_start)
            text += " | Range start set";
        std::string hint;
        auto inspected_command = highlighted_menu_command != ckv::ui::kInvalidCommand
            ? highlighted_menu_command : pressed_footer_command;
        if (i == active && palette_focused) {
            if (const auto *palette = dynamic_cast<ckv::widgets::CommandPalette *>(application.focused())) {
                if (const auto highlighted = palette->highlighted_command())
                    inspected_command = *highlighted;
                else if (const auto filtered = palette->filtered_commands(); !filtered.empty())
                    inspected_command = filtered.front().id;
            }
        }
        if (i == active && inspected_command != ckv::ui::kInvalidCommand) {
            if (const auto *command = application.commands().find(inspected_command))
                hint = availability(command->key).reason;
        } else if (i == active && p.volume_view) {
            // A volume row has one meaningful operation: archive Copy. Keep
            // its reason visible beside the row so the disabled menu, palette
            // and keyboard command never leave the user guessing.
            hint = availability("ck.files.copy").reason;
        } else if (i == active && volume_archive_recovery_cleanup_request) {
            hint = "Archive staging cleanup is running";
        } else if (i == active && !volume_archive_recovery_cleanup_error.empty()) {
            hint = volume_archive_recovery_cleanup_error;
        } else if (i == active && operation_recovery_cleanup_request) {
            hint = "Operation staging cleanup is running";
        } else if (i == active && !operation_recovery_cleanup_error.empty()) {
            hint = operation_recovery_cleanup_error;
        } else if (i == active && volume_archive_recovery_scan &&
                   volume_archive_recovery_scan->requires_reconciliation) {
            hint = "Inspect archive recovery: review is required before volume Copy";
        } else if (i == active && operation_recovery_scan &&
                   operation_recovery_scan->requires_reconciliation) {
            hint = "Inspect operation recovery: review is required";
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
                "ck.files.copy", "ck.files.move", "ck.files.trash", "ck.files.permanently_delete"};
            const auto report_first_unavailable = [&](const auto &contextual) {
                for (const auto command : contextual) {
                    const auto candidate = availability(command);
                    if (!candidate.enabled) {
                        hint = candidate.reason;
                        break;
                    }
                }
            };
            if (p.model.target() || p.selection.origin == ck::files::PaneSourcesOrigin::Marks)
                report_first_unavailable(selected_contextual);
            else
                report_first_unavailable(empty_listing_contextual);
        }
        if (!hint.empty() && !text.starts_with(hint))
            text += "\nHint: " + hint;
        if (!settings.diagnostic.empty())
            text += " | " + settings.diagnostic;
        return text;
    }
    bool archive_recovery_open() const {
        return archive_recovery_window && !archive_recovery_lifetime.expired();
    }
    bool reconciliation_overlap_open() const {
        return reconciliation_overlap_window && !reconciliation_overlap_lifetime.expired();
    }
    static const char *transfer_disposition_label(ck::files::TransferRequestDisposition disposition) noexcept {
        using D = ck::files::TransferRequestDisposition;
        switch (disposition) {
        case D::ReviewRecordedAttempt: return "review recorded attempt";
        case D::ReviewNewLinkedAttempt: return "review new linked attempt";
        case D::ReviewBlockedRetry: return "blocked retry";
        case D::ReviewOverlappingAttempts: return "overlapping transfer attempts";
        case D::ReviewUnknownScope: return "unknown transfer scope";
        case D::PreserveDestinationOnly: return "preserve destination-only entry";
        case D::ReviewObservedComparison: return "review observed comparison";
        }
        return "unknown transfer scope";
    }
    static const char *transfer_row_review_label(ck::files::TransferRowReview review) noexcept {
        using R = ck::files::TransferRowReview;
        switch (review) {
        case R::RevalidateOriginalScope: return "revalidate original scope";
        case R::PreserveUnpublishedStaging: return "preserve unpublished staging";
        case R::ReviewVerifiedStaging: return "review verified staging";
        case R::InspectAmbiguousPublication: return "inspect ambiguous publication";
        case R::VerifyAndProtectCommittedDestination: return "verify and protect committed destination";
        case R::InspectAmbiguousSourceRetirement: return "inspect ambiguous source retirement";
        case R::RequireEvidenceReview: return "require evidence review";
        }
        return "require evidence review";
    }
    static const char *transfer_lineage_label(ck::files::TransferLineageStatus lineage) noexcept {
        using L = ck::files::TransferLineageStatus;
        switch (lineage) {
        case L::Unrecorded: return "unrecorded";
        case L::ParentAndScopeMatched: return "parent and scope matched";
        case L::Malformed: return "malformed retry provenance";
        case L::UnsupportedVersion: return "unsupported retry version";
        case L::DuplicateJournalId: return "duplicate journal identifier";
        case L::ParentMissing: return "parent journal missing";
        case L::Cycle: return "retry lineage cycle";
        case L::ParentBytesUnverified: return "parent journal bytes unverified";
        case L::ParentItemMissing: return "parent item missing";
        case L::ParentItemIneligible: return "parent item ineligible";
        case L::ScopeUnverified: return "scope unverified";
        case L::ScopeMismatch: return "scope mismatch";
        case L::InvalidAncestor: return "invalid ancestor";
        }
        return "unrecorded";
    }
    void show_reconciliation_overlap(ck::files::FileOperation operation,
                                     const FilesOperationRecoveryOverlapReport &report) {
        if (modal_pending() || !report.has_conflict())
            return;
        using namespace ckv::widgets;
        const std::string op_label = operation == ck::files::FileOperation::Move ? "Move"
            : operation == ck::files::FileOperation::Delete ? "Delete" : "Copy";
        std::ostringstream detail;
        detail << op_label << " is held because an existing operation journal overlaps this transfer.\n\n"
               << "Original-job resume and linked retry require qualified reconciliation and cannot be "
                  "bypassed by starting a new " << op_label << ". Source, destination, and existing "
                  "journals were left unchanged.\n";
        for (const auto &conflict : report.conflicts) {
            detail << '\n';
            switch (conflict.kind) {
            case FilesOperationRecoveryOverlapKind::UnfinishedTransfer:
                detail << "Type: unfinished transfer\n";
                break;
            case FilesOperationRecoveryOverlapKind::UnfinishedDeletion:
                detail << "Type: unfinished deletion\n";
                break;
            case FilesOperationRecoveryOverlapKind::RetainedSourceRepeat:
                detail << "Type: retained-source repeat\n";
                break;
            case FilesOperationRecoveryOverlapKind::UntrustedJournal:
                detail << "Type: untrusted or unreadable journal\n";
                break;
            case FilesOperationRecoveryOverlapKind::UnqualifiedJournal:
                detail << "Type: unfinished journal without native identities\n";
                break;
            }
            if (!conflict.journal_name.empty())
                detail << "Journal: " << conflict.journal_name << '\n';
            if (conflict.state)
                detail << "State: " << archive_recovery_state(*conflict.state);
            if (conflict.action)
                detail << (conflict.state ? "; action: " : "Action: ")
                       << archive_recovery_action(*conflict.action);
            if (conflict.state || conflict.action)
                detail << '\n';
            detail << "Reason: " << conflict.reason << '\n';
            if (conflict.disposition)
                detail << "Disposition: " << transfer_disposition_label(*conflict.disposition) << '\n';
            if (conflict.row_review)
                detail << "Review: " << transfer_row_review_label(*conflict.row_review) << '\n';
            if (conflict.lineage && *conflict.lineage != ck::files::TransferLineageStatus::Unrecorded)
                detail << "Lineage: " << transfer_lineage_label(*conflict.lineage) << '\n';
            if (conflict.source_name)
                detail << "Source: " << *conflict.source_name << '\n';
            if (conflict.destination_name)
                detail << "Destination: " << *conflict.destination_name << '\n';
            if (conflict.destination_address)
                detail << "Recorded folder: " << *conflict.destination_address << '\n';
        }
        auto owner = std::make_unique<Window>("Reconciliation required");
        auto &desktop = shell->desktop();
        const auto &roles = shell->roles();
        const auto area = desktop.content_area();
        const int width = std::min(90, area.width), height = std::min(22, area.height);
        owner->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
        owner->set_min_size({24, 6});
        owner->set_resizable(true);
        owner->set_content_margin(1, 1);
        owner->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
        auto content = std::make_unique<ckv::ui::Column>();
        auto reader = std::make_unique<TextView>();
        reader->set_wrap_mode(WrapMode::Character);
        reader->set_text(detail.str());
        auto *initial = reader.get();
        content->add_item(std::move(reader), {.policy = ckv::ui::SizePolicy::Expanding});
        auto buttons = std::make_unique<ckv::ui::Row>();
        buttons->set_spacing(1);
        auto *const raw_owner = owner.get();
        const auto close = [raw_owner] { raw_owner->close(); };
        owner->cancel_request = close;
        if (operation_recovery_service && operation_journal_root) {
            if (availability("ck.files.resume_operation_recovery").enabled) {
                auto resume_btn = std::make_unique<Button>("&Resume recorded job");
                resume_btn->on_press = [this, close, weak = std::weak_ptr<void>(lifetime)] {
                    close();
                    if (!weak.expired())
                        execute_first_recovery_action(FilesOperationRecoveryActionKind::ResumeRecordedJob);
                };
                buttons->add_item(std::move(resume_btn));
            }
            if (availability("ck.files.retry_operation_recovery").enabled) {
                auto retry_btn = std::make_unique<Button>("Re&try unresolved items");
                retry_btn->on_press = [this, close, weak = std::weak_ptr<void>(lifetime)] {
                    close();
                    if (!weak.expired())
                        execute_first_recovery_action(FilesOperationRecoveryActionKind::RetryUnresolvedItems);
                };
                buttons->add_item(std::move(retry_btn));
            }
            auto inspect = std::make_unique<Button>("&Inspect operation recovery");
            inspect->on_press = [this, close, weak = std::weak_ptr<void>(lifetime)] {
                close();
                if (!weak.expired() && availability("ck.files.inspect_operation_recovery").enabled)
                    show_operation_recovery();
            };
            buttons->add_item(std::move(inspect));
        }
        auto dismiss = std::make_unique<Button>("&Close");
        dismiss->on_press = close;
        buttons->add_item(std::move(dismiss));
        content->add_item(std::move(buttons));
        owner->set_content(std::move(content));
        reconciliation_overlap_window = desktop.present_modal(WindowHandle{std::move(owner), initial}, application);
        if (reconciliation_overlap_window) {
            reconciliation_overlap_lifetime = reconciliation_overlap_window->lifetime_token();
            reconciliation_overlap_window->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
                if (!weak.expired()) {
                    reconciliation_overlap_window = nullptr;
                    if (!modal_pending() && surfaces[active] && surfaces[active]->grid)
                        application.set_focus(surfaces[active]->grid);
                    labels();
                }
            };
            labels();
        }
    }
    void show_archive_recovery() {
        if (!availability("ck.files.inspect_archive_recovery").enabled ||
            !volume_archive_recovery_scan || modal_pending())
            return;
        using namespace ckv::widgets;
        auto model = std::make_shared<ArchiveRecoveryList>(volume_archive_recovery_scan);
        auto owner = std::make_unique<Window>("Archive recovery review");
        auto &desktop = shell->desktop();
        const auto &roles = shell->roles();
        const auto area = desktop.content_area();
        const int width = std::min(90, area.width), height = std::min(22, area.height);
        owner->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
        owner->set_min_size({24, 6});
        owner->set_resizable(true);
        owner->set_content_margin(1, 1);
        owner->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
        auto content = std::make_unique<ckv::ui::Column>();
        content->add_item(std::make_unique<Label>(
            "Read-only inspection | Journals: " + std::to_string(model->item_count()) +
            " | Need review: " + std::to_string(model->review_count())));
        auto list = std::make_unique<ListView>();
        auto *const selection = list.get();
        list->set_model(*model);
        list->on_activate = [this, model](std::size_t index) {
            if (index < model->item_count())
                present_files_read_only_text("Archive recovery details", model->details(index),
                    application, shell->desktop(), shell->roles());
        };
        content->add_item(std::move(list), {.policy = ckv::ui::SizePolicy::Expanding});
        auto buttons = std::make_unique<ckv::ui::Row>();
        buttons->set_spacing(1);
        auto inspect = std::make_unique<Button>("&Details");
        inspect->on_press = [selection] {
            if (const auto index = selection->cursor(); index >= 0)
                selection->on_activate(static_cast<std::size_t>(index));
        };
        buttons->add_item(std::move(inspect));
        auto dismiss = std::make_unique<Button>("&Close");
        owner->cancel_request = [view = owner.get()] { view->close(); };
        dismiss->on_press = owner->cancel_request;
        buttons->add_item(std::move(dismiss));
        content->add_item(std::move(buttons));
        owner->set_content(std::move(content));
        archive_recovery_window = desktop.present_modal(WindowHandle{std::move(owner), selection}, application);
        if (archive_recovery_window) {
            archive_recovery_lifetime = archive_recovery_window->lifetime_token();
            archive_recovery_window->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
                if (!weak.expired()) {
                    archive_recovery_window = nullptr;
                    labels();
                }
            };
            labels();
        }
    }
    void show_archive_cleanup() {
        volume_archive_recovery_cleanup_error = availability("ck.files.cleanup_archive_recovery").reason;
        labels();
    }
    std::vector<ck::files::NativeAbsolutePath> candidate_pane_directories() const {
        std::vector<ck::files::NativeAbsolutePath> dirs;
        for (std::size_t i = 0; i < 2; ++i) {
            if (!panes[i].active)
                continue;
            auto path = panes[i].active->root_address();
            bool ok = true;
            for (const auto &component : panes[i].active->components()) {
                auto child = path.child_address(component);
                if (!child.value()) { ok = false; break; }
                path = std::move(child).take_value();
            }
            if (ok && path.is_valid())
                dirs.push_back(std::move(path));
        }
        return dirs;
    }
    bool execute_recovery_action(std::size_t journal_index, FilesOperationRecoveryActionKind kind) {
        if (!operation_recovery_service || !operation_journal_root || !operation_recovery_scan ||
            operation_recovery_request || operation_recovery_action_pending)
            return false;
        if (journal_index >= operation_recovery_scan->journals.size())
            return false;
        const auto &item = operation_recovery_scan->journals[journal_index];
        if (!item.native_name || !item.report)
            return false;
        const bool want_retention = kind == FilesOperationRecoveryActionKind::RestoreRetainedTarget ||
                                    kind == FilesOperationRecoveryActionKind::DisposeRetainedTarget;
        if ((item.kind == FilesOperationJournalKind::RetainedTarget) != want_retention) {
            operation_recovery_action_status = want_retention
                ? "Selected journal is a transfer; use Resume or Retry"
                : "Selected journal is a retained target; use Restore or Dispose";
            labels();
            return false;
        }
        FilesOperationRecoveryActionRequest req{
            kind,
            *operation_journal_root,
            *item.native_name,
            item.report->journal_id,
            candidate_pane_directories(),
            {}};
        const auto submitted = operation_recovery_service->request_action(std::move(req));
        if (submitted != FilesOperationRecoverySubmission::Accepted) {
            operation_recovery_action_status = "Operation recovery action rejected";
            labels();
            return false;
        }
        operation_recovery_action_pending = true;
        operation_recovery_action_status = "Executing recovery action...";
        watch();
        labels();
        return true;
    }
    bool execute_first_recovery_action(FilesOperationRecoveryActionKind kind) {
        if (!operation_recovery_scan)
            return false;
        const bool want_retention = kind == FilesOperationRecoveryActionKind::RestoreRetainedTarget ||
                                    kind == FilesOperationRecoveryActionKind::DisposeRetainedTarget;
        for (std::size_t i = 0; i < operation_recovery_scan->journals.size(); ++i) {
            const auto &item = operation_recovery_scan->journals[i];
            if (item.native_name && item.report &&
                (item.kind == FilesOperationJournalKind::RetainedTarget) == want_retention)
                return execute_recovery_action(i, kind);
        }
        return false;
    }
    void show_operation_recovery() {
        if (!availability("ck.files.inspect_operation_recovery").enabled ||
            !operation_recovery_scan || modal_pending())
            return;
        using namespace ckv::widgets;
        std::ostringstream summary;
        summary << "Read-only inspection\n";
        if (operation_recovery_scan->reconciliation_error) {
            summary << "Reconciliation catalog: review required ("
                    << operation_recovery_scan->reconciliation_error->error.operation << ")\n\n";
        }
        constexpr std::size_t max_items = 64;
        const auto &journals = operation_recovery_scan->journals;
        const auto count = std::min(max_items, journals.size());
        auto remaining_items = max_items;
        for (std::size_t index = 0; index != count; ++index) {
            const auto &item = journals[index];
            summary << "Journal: " << item.display_name << '\n';
            const bool retention = item.kind == FilesOperationJournalKind::RetainedTarget;
            if (retention)
                summary << "Previous target version\n";
            if (item.reconciliation &&
                item.reconciliation->lineage != ck::files::TransferLineageStatus::Unrecorded) {
                summary << "Lineage: " << transfer_lineage_label(item.reconciliation->lineage) << '\n';
            }
            if (item.report) {
                const auto &inspection = item.report->inspection;
                summary << "Items: " << inspection.report.items.size() << '\n';
                const auto shown = std::min(remaining_items, inspection.report.items.size());
                auto artifact = inspection.artifacts.begin();
                for (std::size_t row = 0; row != shown; ++row) {
                    const auto &recovery = inspection.report.items[row];
                    summary << "State: " << archive_recovery_state(recovery.state)
                            << "; action: " << archive_recovery_action(recovery.action) << '\n';
                    if (item.reconciliation) {
                        for (const auto &rec_row : item.reconciliation->items) {
                            if (rec_row.item == recovery.item) {
                                summary << "Review: " << transfer_row_review_label(rec_row.review) << '\n';
                                break;
                            }
                        }
                    }
                    while (artifact != inspection.artifacts.end() && artifact->item < recovery.item)
                        ++artifact;
                    if (artifact != inspection.artifacts.end() && artifact->item == recovery.item && artifact->evidence) {
                        const auto &entry = *artifact->evidence;
                        if (retention) {
                            const bool recorded = recovery.state == ck::files::OperationJournalState::Complete &&
                                !inspection.has_untrusted_evidence;
                            summary << "Original target: " << entry.source_name.value_or(entry.staging_name).display_name()
                                    << '\n' << (recorded ? "Recorded retained name: " : "Proposed retained name: ")
                                    << entry.destination_name.display_name() << '\n';
                            if (entry.destination_address)
                                summary << "Recorded folder: " << entry.destination_address->display_text() << '\n';
                        } else if (entry.source_name) {
                            summary << "Source: " << entry.source_name->display_name() << '\n'
                                    << "Destination: " << entry.destination_name.display_name() << '\n';
                        }
                    }
                }
                remaining_items -= shown;
                if (shown != inspection.report.items.size())
                    summary << "Additional items: " << inspection.report.items.size() - shown << '\n';
                if (inspection.report.has_untrusted_tail)
                    summary << "Journal tail: untrusted\n";
                if (inspection.has_untrusted_evidence)
                    summary << "Native evidence: review required\n";
            } else if (item.error) {
                summary << "Inspection error: " << item.error->error.operation << '\n';
            }
            summary << '\n';
        }
        if (journals.size() > count)
            summary << "Additional journal entries omitted from this bounded view: "
                    << (journals.size() - count) << '\n';

        auto model = std::make_shared<OperationRecoveryList>(
            std::make_shared<const FilesOperationRecoveryScan>(*operation_recovery_scan));
        auto owner = std::make_unique<Window>("Operation recovery review");
        auto &desktop = shell->desktop();
        const auto &roles = shell->roles();
        const auto area = desktop.content_area();
        const int width = std::min(96, area.width), height = std::min(24, area.height);
        owner->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
        owner->set_min_size({36, 10});
        owner->set_resizable(true);
        owner->set_content_margin(1, 1);
        owner->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
        auto content = std::make_unique<ckv::ui::Column>();
        auto reader = std::make_unique<TextView>();
        reader->set_wrap_mode(WrapMode::Character);
        reader->set_text(summary.str());
        content->add_item(std::move(reader), {.policy = ckv::ui::SizePolicy::Expanding, .weight = 5});
        auto list = std::make_unique<ListView>();
        auto *const selection = list.get();
        list->set_model(*model);
        list->on_activate = [this, model](std::size_t index) {
            if (index < model->item_count())
                present_files_read_only_text("Operation recovery details", model->details(index),
                    application, shell->desktop(), shell->roles());
        };
        content->add_item(std::move(list), {.policy = ckv::ui::SizePolicy::Expanding, .weight = 1});
        auto buttons = std::make_unique<ckv::ui::Row>();
        buttons->set_spacing(1);
        auto *const raw_owner = owner.get();
        const auto close = [raw_owner] { raw_owner->close(); };
        owner->cancel_request = close;
        const auto invoke = [this, selection, close, weak = std::weak_ptr<void>(lifetime)](
                                FilesOperationRecoveryActionKind kind) {
            const auto idx = selection->cursor();
            if (idx < 0)
                return;
            close();
            if (!weak.expired())
                execute_recovery_action(static_cast<std::size_t>(idx), kind);
        };
        auto resume_btn = std::make_unique<Button>("&Resume");
        resume_btn->on_press = [invoke] { invoke(FilesOperationRecoveryActionKind::ResumeRecordedJob); };
        buttons->add_item(std::move(resume_btn));
        auto retry_btn = std::make_unique<Button>("Re&try");
        retry_btn->on_press = [invoke] { invoke(FilesOperationRecoveryActionKind::RetryUnresolvedItems); };
        buttons->add_item(std::move(retry_btn));
        auto restore_btn = std::make_unique<Button>("Re&store");
        restore_btn->on_press = [invoke] { invoke(FilesOperationRecoveryActionKind::RestoreRetainedTarget); };
        buttons->add_item(std::move(restore_btn));
        auto dispose_btn = std::make_unique<Button>("&Dispose");
        dispose_btn->on_press = [invoke] { invoke(FilesOperationRecoveryActionKind::DisposeRetainedTarget); };
        buttons->add_item(std::move(dispose_btn));
        auto dismiss = std::make_unique<Button>("&Close");
        dismiss->on_press = close;
        buttons->add_item(std::move(dismiss));
        content->add_item(std::move(buttons));
        owner->set_content(std::move(content));
        operation_recovery_window = desktop.present_modal(WindowHandle{std::move(owner), selection}, application);
        if (operation_recovery_window) {
            operation_recovery_lifetime = operation_recovery_window->lifetime_token();
            operation_recovery_window->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
                if (!weak.expired()) {
                    operation_recovery_window = nullptr;
                    if (!modal_pending() && surfaces[active] && surfaces[active]->grid)
                        application.set_focus(surfaces[active]->grid);
                    labels();
                }
            };
            labels();
        }
    }
    void show_operation_recovery_cleanup() {
        // Recovered facts cannot authorize deletion of a staging object.
        operation_recovery_cleanup_error = availability("ck.files.cleanup_operation_recovery").reason;
        labels();
    }
    FilesSettingsSnapshotView settings_snapshot() const noexcept {
        FilesSettingsSnapshotView value;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            value.views[i] = &panes[i].view;
            value.bookmarks[i] = panes[i].bookmarks;
        }
        return value;
    }
    void poll_settings() {
        if (!settings.worker)
            return;
        auto reply = settings.worker->take();
        if (!reply && settings_waiting)
            reply = settings.worker->latest_result(); // Retry UI formatting after allocation failure.
        if (reply && reply->revision == settings_revision) {
            if (!settings_feedback_current) {
                settings_waiting = false;
                return; // A later rejected UI intent owns the diagnostic.
            }
            auto diagnostic = reply->message().empty()
                ? std::string(files_settings_write_message(reply->state)) : std::string(reply->message());
            if (reply->diagnostic_truncated)
                diagnostic += " [additional diagnostic text omitted]";
            settings.diagnostic.swap(diagnostic);
            settings_waiting = settings_feedback_current = false;
            labels();
        }
    }
    void save_settings() {
        if (settings.worker) {
            const auto submitted = settings.worker->submit(settings_snapshot());
            if (submitted.state == FilesSettingsAdmission::Accepted) {
                settings_revision = submitted.revision;
                settings_waiting = settings_feedback_current = true;
                settings.diagnostic = "Saving settings";
            } else {
                settings_feedback_current = false;
                settings.diagnostic = files_settings_admission_message(submitted.state);
            }
            watch(); // Covers a failed wake even if save already completed.
            labels();
            return;
        }
        // Deliberately disabled for headless/injected controllers. Production
        // supplies a worker. No callback fallback and no claim of persistence.
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
        const auto pane_items = [this](std::size_t i) {
            const auto &pane = panes[i];
            if (volume_pending(i) || (pane.volume_view && pane.navigating))
                return std::string("loading");
            if (pane.volume_view && pane.volume_inventory_stale)
                return std::string("stale");
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
        else if (operation_recovery_action_pending)
            activity = "Recovering operation";
        else if (operation_planning_request)
            activity = operation_planning_cancelling ? "Cancelling plan" : "Planning operation";
        else if (operation_execution_request)
            activity = operation_execution_origin->kind == ck::files::FileOperation::Move
                ? "Moving"
                : operation_execution_origin->kind == ck::files::FileOperation::Delete
                    ? "Deleting" : "Copying";
        else if (volume_pending(0) || volume_pending(1))
            activity = "Reading volumes";
        else if (modal_pending())
            activity = "Dialog open";
        else if (pending(panes[0]) || pending(panes[1]))
            activity = "Scanning";
        else if (!operation_recovery_action_status.empty())
            activity = operation_recovery_action_status;
        else
            activity = "Ready";
        window->set_footer("Active: " + std::string(active == 0 ? "Left" : "Right") +
                           " | Items: L " + pane_items(0) + " / R " + pane_items(1) +
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
            !panes[i].active->components().empty() || volume_request || modal_pending())
            return false;
        if (!volume_service->request()) {
            panes[i].error = "Volume information is busy";
            labels();
            return false;
        }
        volume_request = VolumeRequest{i, panes[i].navigation};
        if (panes[i].volume_view) {
            panes[i].volume_inventory_stale = true;
            if (volume_archive_recovery_service)
                scan_archive_recovery();
        }
        watch();
        labels();
        return true;
    }
    void consume_volume(FilesVolumeReply reply) {
        if (!volume_request)
            return;
        const auto pane = volume_request->pane;
        const auto current = volume_pending(pane);
        volume_request.reset();
        if (!current)
            return;
        if (reply.state != FilesVolumeReplyState::Ready) {
            panes[pane].error = volume_failure(reply);
            labels();
            return;
        }
        panes[pane].volume_view = true;
        panes[pane].volume_inventory_stale = false;
        panes[pane].model.publish_volumes(std::move(reply.volumes));
        surfaces[pane]->grid->model_changed();
        panes[pane].error.clear();
        panes[pane].navigation_error = false;
        labels();
    }
    bool relative_ready(std::size_t i, bool root) const {
        if (!ready(i) || !panes[i].active)
            return false;
        const auto next = root ? panes[i].active->namespace_root() : panes[i].active->parent();
        if (!next.value())
            return false;
        if (*next.value() == *panes[i].active)
            return !root && !volume_request && panes[i].active->components().empty() &&
                   volume_service && volume_service->running();
        return true;
    }
    bool navigate_relative(std::size_t i, bool root) {
        if (i < 2 && !root && panes[i].volume_view) {
            if (closing || modal_pending() || panes[i].navigating || !services[i]->running())
                return false;
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
    FilesCommandAvailability refresh_availability(std::size_t i) const {
        if (i < panes.size() && panes[i].volume_view) {
            if (panes[i].navigating)
                return FilesCommandAvailability::unavailable("Refresh unavailable: active pane is opening a location");
            if (volume_request)
                return FilesCommandAvailability::unavailable("Refresh unavailable: volume information is still loading");
            if (!volume_service || !volume_service->running())
                return FilesCommandAvailability::unavailable("Refresh unavailable: volume information worker is unavailable");
            if (!closing && !modal_pending())
                return FilesCommandAvailability::available();
        }
        if (i >= panes.size() || !services[i]->can_refresh())
            return FilesCommandAvailability::unavailable("Refresh unavailable: held directory scans are unsupported");
        return ready(i, false, true) ? FilesCommandAvailability::available()
                                    : FilesCommandAvailability::unavailable("Refresh unavailable: active pane is not ready");
    }
    bool refresh(std::size_t i) {
        if (!refresh_availability(i).enabled)
            return false;
        auto &p = panes[i];
        if (p.volume_view)
            return show_volume_info(i);
        if (!p.refresh_failed) {
            if (auto prepared = prepare_current_row(i)) {
                p.queued_refresh = *prepared;
                labels();
                return *prepared;
            }
        }
        return start_refresh(i);
    }
    bool start_refresh(std::size_t i) {
        auto &p = panes[i];
        p.history_target.reset();
        const auto submitted = services[i]->refresh(p.navigation, p.stamp, p.model.viewport().rows);
        return accept_navigation(i, submitted, p.active, false, true);
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
        const auto bounds = validate_files_settings_snapshot(settings_snapshot(), i, panes[i].active.get());
        if (bounds != FilesSettingsAdmission::Accepted) {
            settings_feedback_current = false;
            settings.diagnostic = files_settings_admission_message(bounds);
            labels();
            return false; // No partial bookmark insertion or silent truncation.
        }
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
                           std::shared_ptr<const FilesPaneLocation> requested, bool home = false, bool refreshing = false) {
        auto &p = panes[i];
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = refreshing ? "Refresh request rejected" : "Navigation request rejected";
            labels();
            return false;
        }
        p.archive_refresh.reset();
        if (!requested || !p.active || *requested != *p.active)
            p.archive_notice.clear();
        p.navigation = p.request = submitted.ticket;
        p.requested = std::move(requested);
        p.navigating = true;
        p.refreshing = refreshing;
        p.entering = false;
        p.homing = home;
        p.reading = false;
        p.changing.reset();
        p.queued_marks.clear();
        if (!refreshing) {
            p.batch_delete_kind = Pane::BatchDeleteKind::None;
            p.batch_delete_names.clear();
        }
        p.queued_view.reset();
        p.queued_refresh = false;
        p.queued_copy.reset();
        p.inspection.reset();
        // Keep the volume rows visible and inert until the worker accepts a
        // replacement page. Failure or cancellation leaves the selection here.
        p.model.set_input_locked(p.volume_view || refreshing);
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
    bool create_file(std::size_t i, ck::files::NativeName name) {
        if (!ready(i) || !name.is_valid())
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->create_file(p.navigation, p.stamp, std::move(name),
                                                         {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "File creation request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.creating_file = true;
        p.cancelling = false;
        p.error.clear();
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool create_hard_link(std::size_t i, FilesPaneRowTarget source, ck::files::NativeName name) {
        if (!ready(i) || !name.is_valid() || panes[i].model.cursor_kind() != ck::files::ObjectKind::RegularFile)
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->create_hard_link(p.navigation, p.stamp, source.row, std::move(name),
                                                              {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Hard-link creation request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.creating_hard_link = true;
        p.cancelling = false;
        p.error.clear();
        p.model.set_input_locked(true);
        watch();
        labels();
        return true;
    }
    bool create_symbolic_link(std::size_t i, ck::files::SymbolicLinkTarget target, ck::files::NativeName name) {
        if (!ready(i) || !name.is_valid())
            return false;
        auto &p = panes[i];
        const auto range = p.model.viewport();
        const auto submitted = services[i]->create_symbolic_link(p.navigation, p.stamp, std::move(target),
                                                                   std::move(name), {range.first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted) {
            p.error = "Symbolic-link creation request rejected";
            labels();
            return false;
        }
        p.request = submitted.ticket;
        p.mutating = true;
        p.creating_symbolic_link = true;
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
    bool drain_batch_delete(std::size_t i) {
        auto &p = panes[i];
        if (p.batch_delete_kind == Pane::BatchDeleteKind::None || p.batch_delete_names.empty())
            return false;
        auto remaining = std::move(p.batch_delete_names);
        p.batch_delete_names.clear();
        if (!ready(i)) {
            p.batch_delete_names = std::move(remaining);
            return false;
        }
        const auto *rows = p.model.visible_rows();
        if (!rows) {
            p.batch_delete_kind = Pane::BatchDeleteKind::None;
            return false;
        }
        while (!remaining.empty()) {
            const auto expected_name = std::move(remaining.front());
            remaining.erase(remaining.begin());
            for (const auto &row : *rows) {
                if (row.marked && row.display_name == expected_name) {
                    const FilesPaneRowTarget target{p.stamp, row.id};
                    std::optional<ck::files::ObjectKind> row_kind;
                    if (row.metadata.kind.value())
                        row_kind = *row.metadata.kind.value();
                    const bool ok = p.batch_delete_kind == Pane::BatchDeleteKind::Trash
                        ? trash(i, target)
                        : permanently_delete(i, target, row_kind);
                    if (ok) {
                        p.batch_delete_names = std::move(remaining);
                    } else {
                        p.batch_delete_kind = Pane::BatchDeleteKind::None;
                    }
                    return ok;
                }
            }
        }
        p.batch_delete_kind = Pane::BatchDeleteKind::None;
        return false;
    }
    bool start_batch_delete(std::size_t i, Pane::BatchDeleteKind kind) {
        if (!ready(i))
            return false;
        auto &p = panes[i];
        const auto *rows = p.model.visible_rows();
        if (!rows)
            return false;
        std::vector<std::string> names;
        for (const auto &row : *rows) {
            if (row.marked)
                names.push_back(row.display_name);
        }
        if (names.empty())
            return false;
        p.batch_delete_kind = kind;
        p.batch_delete_names = std::move(names);
        return drain_batch_delete(i);
    }
    bool permanently_delete(std::size_t i, FilesPaneRowTarget target,
                            std::optional<ck::files::ObjectKind> target_kind = std::nullopt) {
        if (!ready(i))
            return false;
        auto &p = panes[i];
        const auto effective_kind = target_kind.has_value() ? target_kind : p.model.cursor_kind();
        if (effective_kind == ck::files::ObjectKind::Directory) {
            if (!execution_service || !operation_journal_root || operation_execution_request) {
                p.error = "Permanent deletion unavailable: the recursive deletion worker is not configured";
                labels();
                return false;
            }
            FilesOperationExecutionRequest execution{
                ck::files::FileOperation::Delete, services[i], nullptr,
                p.navigation, {}, p.stamp, {}, {}, *operation_journal_root, {}, {}, {}, {}, {}};
            execution.source_limits.held_entries = 1024;
            execution.source_limits.access = ck::files::EntryAccess::MetadataOnly;
            execution.open_journal = [](const ck::files::NativeAbsolutePath &root,
                                        const ck::files::NativeName &basename, bool create_new) {
                return ck::files::open_native_operation_journal_file(root, basename, create_new);
            };
            const auto submitted = execution_service->request(std::move(execution));
            if (submitted.state != FilesOperationExecutionSubmission::Accepted) {
                p.error = "Permanent deletion worker rejected the request";
                labels();
                return false;
            }
            operation_execution_request = submitted.ticket;
            operation_execution_origin = OperationExecutionOrigin{ck::files::FileOperation::Delete, i, p.navigation, {}};
            p.error.clear();
            watch();
            labels();
            return true;
        }
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
    // The visible cursor can move independently of the indexed worker.
    // A view change or refresh first acknowledges its exact scoped row.
    std::optional<bool> prepare_current_row(std::size_t i) {
        auto &p = panes[i];
        if (p.selection.invalidated_refresh_focus || p.worker_focus == p.model.cursor().row || !p.model.row_count())
            return {};
        if (const auto target = p.model.target())
            return begin_change(i, FilesPaneFocusRow{target->row});
        // Wheel scrolling can leave the cursor outside the displayed cache.
        // Read its exact dataset position before asking the worker to validate
        // a row identity; never manufacture one from the position.
        const auto range = p.model.viewport();
        const auto first = p.model.cursor().row;
        auto submitted = services[i]->read_page(p.navigation, p.stamp, {first, range.rows});
        if (submitted.state != FilesPaneSubmission::Accepted)
            return false;
        p.request = submitted.ticket;
        p.inflight = {first, range.rows};
        p.reading = p.preparing_cursor = true;
        p.cancelling = p.change_error = false;
        p.error.clear();
        p.blocked.reset();
        p.model.set_input_locked(true);
        watch();
        return true;
    }
    bool swap_panes() {
        if (!availability("ck.files.swap_panes").enabled)
            return false;
        auto loc0 = *panes[0].active;
        auto loc1 = *panes[1].active;
        auto view0 = panes[0].view;
        auto view1 = panes[1].view;
        const bool ok0 = navigate(0, std::move(loc1), std::move(view1));
        const bool ok1 = navigate(1, std::move(loc0), std::move(view0));
        return ok0 && ok1;
    }
    bool sync_other_pane() {
        if (!availability("ck.files.sync_other_pane").enabled)
            return false;
        const std::size_t other = 1 - active;
        auto loc = *panes[active].active;
        auto view = panes[other].view;
        return navigate(other, std::move(loc), std::move(view));
    }
    bool drain_queued_marks(std::size_t i) {
        auto &p = panes[i];
        if (p.queued_marks.empty() || p.changing || p.navigating || p.mutating || p.reading)
            return false;
        auto remaining = std::move(p.queued_marks);
        p.queued_marks.clear();
        auto next = std::move(remaining.front());
        remaining.erase(remaining.begin());
        if (!begin_change(i, FilesPaneChangeMarks{std::move(next)}, true)) {
            return false;
        }
        p.queued_marks = std::move(remaining);
        return true;
    }
    bool mark_pattern(std::size_t i, std::string pattern, bool marked) {
        if (i >= 2 || !ready(i) || panes[i].volume_view || pattern.empty())
            return false;
        const auto *rows = panes[i].model.visible_rows();
        if (!rows)
            return false;
        std::vector<ck::files::IndexedMarkCommand> cmds;
        for (const auto &row : *rows) {
            if (row.marked != marked && glob_match(pattern, row.display_name))
                cmds.push_back(ck::files::IndexedMarkRow{row.id, marked});
        }
        if (cmds.empty())
            return true;
        panes[i].queued_marks = std::move(cmds);
        return drain_queued_marks(i);
    }
    void show_mark_pattern(bool marked) {
        if (!availability(marked ? "ck.files.mark_pattern" : "ck.files.unmark_pattern").enabled || modal_pending())
            return;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = marked ? "Mark files by pattern" : "Unmark files by pattern";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "&Pattern (e.g. *.txt, test_?):"}};
        descriptor.buttons = {
            {marked ? "&Mark" : "&Unmark", ckv::widgets::ButtonRole::Accept, {}},
            {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        const std::size_t pane_idx = active;
        mark_pattern_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        mark_pattern_dialog->set_completion_handler([this, pane_idx, marked](const ckv::widgets::DialogResult &res) {
            std::string pat;
            if (res.accepted && !res.values.empty())
                pat = res.values[0];
            mark_pattern_dialog.reset();
            if (res.accepted && !pat.empty())
                mark_pattern(pane_idx, std::move(pat), marked);
            labels();
        });
    }
    bool compare_directories() {
        if (!availability("ck.files.compare_directories").enabled)
            return false;
        const std::size_t other = 1 - active;
        const auto *src_rows = panes[active].model.visible_rows();
        const auto *dst_rows = panes[other].model.visible_rows();
        if (!src_rows || !dst_rows)
            return false;
        using ModTime = std::decay_t<decltype(*src_rows->front().metadata.modified.value())>;
        struct EntrySig {
            std::optional<ck::files::ObjectKind> kind;
            std::optional<std::uint64_t> size;
            std::optional<ModTime> modified;
        };
        std::map<std::string_view, EntrySig> other_entries;
        for (const auto &row : *dst_rows) {
            EntrySig sig;
            if (row.metadata.kind.value()) sig.kind = *row.metadata.kind.value();
            if (row.metadata.logical_size.value()) sig.size = *row.metadata.logical_size.value();
            if (row.metadata.modified.value()) sig.modified = *row.metadata.modified.value();
            other_entries.emplace(row.display_name, sig);
        }
        std::vector<ck::files::IndexedMarkCommand> cmds;
        for (const auto &row : *src_rows) {
            bool should_mark = false;
            const auto it = other_entries.find(row.display_name);
            std::optional<ck::files::ObjectKind> row_kind;
            if (row.metadata.kind.value()) row_kind = *row.metadata.kind.value();
            std::optional<std::uint64_t> row_size;
            if (row.metadata.logical_size.value()) row_size = *row.metadata.logical_size.value();
            std::optional<ModTime> row_mod;
            if (row.metadata.modified.value()) row_mod = *row.metadata.modified.value();
            if (it == other_entries.end()) {
                should_mark = true;
            } else if (it->second.kind != row_kind) {
                should_mark = true;
            } else if (row_kind == ck::files::ObjectKind::RegularFile) {
                if (row_size != it->second.size)
                    should_mark = true;
                else if (row_mod && it->second.modified &&
                         (row_mod->seconds() > it->second.modified->seconds() ||
                          (row_mod->seconds() == it->second.modified->seconds() &&
                           row_mod->nanoseconds() > it->second.modified->nanoseconds())))
                    should_mark = true;
            }
            if (row.marked != should_mark)
                cmds.push_back(ck::files::IndexedMarkRow{row.id, should_mark});
        }
        if (cmds.empty())
            return true;
        panes[active].queued_marks = std::move(cmds);
        return drain_queued_marks(active);
    }
    std::optional<ck::files::NativeAbsolutePath> pane_absolute_path(std::size_t pane_idx) const {
        if (!panes[pane_idx].active)
            return std::nullopt;
        ck::files::NativeAbsolutePath cur = panes[pane_idx].active->root_address();
        for (const auto &comp : panes[pane_idx].active->components()) {
            auto next = cur.child_address(comp);
            if (!next.value())
                return std::nullopt;
            cur = *next.value();
        }
        return cur;
    }
    std::optional<ck::files::NativeAbsolutePath> selected_child_path(std::size_t pane_idx) const {
        const auto base = pane_absolute_path(pane_idx);
        if (!base)
            return std::nullopt;
        const auto row_target = panes[pane_idx].model.target();
        if (!row_target)
            return std::nullopt;
        const auto *rows = panes[pane_idx].model.visible_rows();
        if (!rows)
            return std::nullopt;
        for (const auto &row : *rows) {
            if (row.id == row_target->row) {
                auto nn = detail::native_name_from_utf8(base->encoding(), row.display_name);
                if (!nn)
                    return std::nullopt;
                auto child = base->child_address(*nn);
                if (child.value())
                    return *child.value();
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
    std::optional<std::size_t> active_embedded_editor_index() const {
        const auto *focused = application.focused();
        for (std::size_t i = embedded_editors.size(); i > 0; --i) {
            const auto &ed = embedded_editors[i - 1];
            if (ed.window && !ed.lifetime.expired() && ed.editor &&
                (contains(ed.window, focused) || shell->desktop().active_window() == ed.window))
                return i - 1;
        }
        return std::nullopt;
    }
    ckv::widgets::TextEditor *active_embedded_editor() const {
        const auto idx = active_embedded_editor_index();
        return idx ? embedded_editors[*idx].editor : nullptr;
    }
    std::vector<ckv::widgets::MenuBarItem> build_menus_for_mode(ActiveViewMode mode) const {
        if (mode == ActiveViewMode::Pane)
            return pane_menus;
        using namespace ckv::widgets;
        const auto cmd_item = [](ckv::ui::CommandId id, std::string label) {
            return MenuItem::command(CommandPresentation{id, std::move(label)});
        };
        const auto make_file_menu = [&](std::vector<MenuItem> top_items) {
            if (!top_items.empty())
                top_items.push_back(MenuItem::separator());
            if (shell->return_to_launcher_command() != ckv::ui::kInvalidCommand)
                top_items.push_back(cmd_item(shell->return_to_launcher_command(), "Return to &Launcher"));
            top_items.push_back(cmd_item(application.commands().standard().quit, "E&xit"));
            return MenuBarItem{"&File", std::move(top_items)};
        };
        const auto help_menu = MenuBarItem{"&Help", {cmd_item(shell->about_command(), "&About...")}};
        const auto palette_cmd = application.commands().standard().command_palette;

        switch (mode) {
        case ActiveViewMode::Editor:
            return {
                make_file_menu({cmd_item(editor_save_id, "&Save"), cmd_item(editor_close_id, "&Close editor")}),
                MenuBarItem{"&Edit",
                            {cmd_item(editor_undo_id, "&Undo"), cmd_item(editor_redo_id, "&Redo"),
                             MenuItem::separator(), cmd_item(editor_cut_id, "Cu&t"),
                             cmd_item(editor_copy_id, "&Copy"), cmd_item(editor_paste_id, "&Paste"),
                             MenuItem::separator(), cmd_item(editor_find_id, "&Find selection"),
                             cmd_item(editor_find_next_id, "Find &next")}},
                MenuBarItem{"&View",
                            {cmd_item(editor_wrap_id, "&Word wrap"),
                             cmd_item(editor_line_numbers_id, "&Line numbers"), MenuItem::separator(),
                             cmd_item(palette_cmd, "&Commands...")}},
                help_menu};
        case ActiveViewMode::PreviewText:
            return {
                make_file_menu({cmd_item(preview_close_id, "&Close preview")}),
                MenuBarItem{"&View",
                            {cmd_item(preview_show_hex_id, "Show &hex"), MenuItem::separator(),
                             cmd_item(palette_cmd, "&Commands...")}},
                help_menu};
        case ActiveViewMode::PreviewHex:
            return {
                make_file_menu({cmd_item(preview_close_id, "&Close preview")}),
                MenuBarItem{"&View",
                            {cmd_item(preview_show_text_id, "Show &text"), MenuItem::separator(),
                             cmd_item(palette_cmd, "&Commands...")}},
                help_menu};
        case ActiveViewMode::PreviewHexOnly:
            return {
                make_file_menu({cmd_item(preview_close_id, "&Close preview")}),
                MenuBarItem{"&View", {cmd_item(palette_cmd, "&Commands...")}},
                help_menu};
        case ActiveViewMode::Terminal:
            return {
                make_file_menu({cmd_item(terminal_close_id, "&Close terminal")}),
                MenuBarItem{"&Terminal",
                            {cmd_item(terminal_focus_files_id, "&Return to Files"),
                             cmd_item(terminal_close_id, "&Close terminal")}},
                help_menu};
        case ActiveViewMode::Report:
            return {
                make_file_menu({cmd_item(report_close_id, "&Close view")}),
                MenuBarItem{"&View", {cmd_item(palette_cmd, "&Commands...")}},
                help_menu};
        case ActiveViewMode::Pane:
            break;
        }
        return pane_menus;
    }
    ActiveViewMode detect_active_view_mode() const {
        const auto *focused = application.focused();
        if (preview_open() &&
            (contains(preview_dialog_window, focused) || shell->desktop().active_window() == preview_dialog_window)) {
            if (!preview_can_toggle)
                return ActiveViewMode::PreviewHexOnly;
            return preview_is_hex ? ActiveViewMode::PreviewHex : ActiveViewMode::PreviewText;
        }
        if (active_embedded_editor() != nullptr)
            return ActiveViewMode::Editor;
        if (embedded_terminal_window && !embedded_terminal_lifetime.expired() &&
            (contains(embedded_terminal_window, focused) ||
             shell->desktop().active_window() == embedded_terminal_window))
            return ActiveViewMode::Terminal;
        if (parity_report_window && !parity_report_lifetime.expired() &&
            (contains(parity_report_window, focused) || shell->desktop().active_window() == parity_report_window))
            return ActiveViewMode::Report;
        return ActiveViewMode::Pane;
    }
    void sync_active_view_chrome() {
        if (closing || !shell || !shell->menu_bar())
            return;
        if (shell->menu_bar()->active())
            return;
        const ActiveViewMode next = detect_active_view_mode();
        if (next != active_view_mode) {
            active_view_mode = next;
            shell->menu_bar()->set_menus(build_menus_for_mode(next));
        }
        if (auto *status = shell->status_line())
            status->invalidate();
    }
    void show_parity_report(std::string title, std::string body) {
        using namespace ckv::widgets;
        if (parity_report_window && !parity_report_lifetime.expired())
            parity_report_window->close();
        auto win = std::make_unique<Window>(std::move(title));
        win->set_command_context("ck.files.report");
        auto content = std::make_unique<TextView>();
        content->set_command_context("ck.files.report");
        content->set_text(std::move(body));
        auto *raw_content = content.get();
        auto *raw_win = win.get();
        win->cancel_request = [raw_win] { raw_win->close(); };
        win->on_detached = [this, weak = std::weak_ptr<void>(lifetime), raw_win] {
            if (weak.expired())
                return;
            if (parity_report_window == raw_win)
                parity_report_window = nullptr;
            if (surfaces[active] && surfaces[active]->grid)
                application.set_focus(surfaces[active]->grid);
            sync_active_view_chrome();
            labels();
        };
        win->set_content(std::move(content));
        const auto desk = shell->desktop().bounds();
        const int w = std::max(44, std::min(96, desk.width - 4));
        const int h = std::max(12, std::min(28, desk.height - 4));
        win->set_bounds({(desk.width - w) / 2, (desk.height - h) / 2, w, h});
        parity_report_window = win.get();
        parity_report_lifetime = win->lifetime_token();
        shell->desktop().add_window(std::move(win));
        application.set_focus(raw_content);
        sync_active_view_chrome();
    }
    bool show_search() {
        if (!availability("ck.files.search").enabled)
            return false;
        ckv::widgets::DialogDescriptor desc;
        desc.title = "Search files (CP-07)";
        desc.fields = {
            ckv::widgets::FieldDescriptor{.label = "&Name pattern (glob):"},
            ckv::widgets::FieldDescriptor{.label = "&Content text (optional):"}};
        desc.buttons = {
            {"&Search", ckv::widgets::ButtonRole::Accept, {}},
            {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        const std::size_t pane_idx = active;
        const auto root_opt = pane_absolute_path(pane_idx);
        if (!root_opt)
            return false;
        const auto root = *root_opt;
        parity_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(desc), application, shell->desktop()));
        parity_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane_idx, root](const ckv::widgets::DialogResult &res) {
            if (weak.expired())
                return;
            std::string name_pat;
            std::string content_pat;
            if (res.accepted && !res.values.empty())
                name_pat = res.values[0];
            if (res.accepted && res.values.size() > 1)
                content_pat = res.values[1];
            parity_dialog.reset();
            if (!res.accepted) {
                labels();
                return;
            }
            FilesSearchQuery query;
            query.root = root;
            query.name_pattern = name_pat;
            query.name_mode = name_pat.empty() ? FilesSearchNameMode::None : FilesSearchNameMode::Glob;
            query.content_query = content_pat;
            const auto sr = run_files_search(query);
            std::ostringstream out;
            out << "Root: " << root.display_text() << "\n"
                << "Matches: " << sr.hits.size()
                << " (scanned " << sr.scanned_entries << " entries in "
                << sr.scanned_directories << " directories)\n";
            if (!sr.error_message.empty())
                out << "Error: " << sr.error_message << "\n";
            if (sr.truncated)
                out << "(Result list truncated at safety limit)\n";
            out << "\n";
            const std::size_t limit = std::min<std::size_t>(sr.hits.size(), 200);
            for (std::size_t idx = 0; idx < limit; ++idx) {
                out << sr.hits[idx].relative_display << " (" << sr.hits[idx].logical_size << " B)";
                if (sr.hits[idx].content_match_line)
                    out << " [L" << *sr.hits[idx].content_match_line << ": " << sr.hits[idx].content_match_preview << "]";
                out << "\n";
            }
            if (!sr.hits.empty() && sr.hits.front().parent_directory)
                navigate(pane_idx, *sr.hits.front().parent_directory);
            show_parity_report("Search results", out.str());
            labels();
        });
        return true;
    }
    bool show_directory_tree() {
        if (!availability("ck.files.directory_tree").enabled)
            return false;
        const auto root_opt = pane_absolute_path(active);
        if (!root_opt)
            return false;
        const auto &root = *root_opt;
        const auto tree = build_files_directory_tree(root, 512, 8);
        std::ostringstream out;
        out << "Directory tree & sizes (ck_du_core): " << root.display_text() << "\n\n";
        if (!tree.error_message.empty())
            out << "Error: " << tree.error_message << "\n";
        for (const auto &node : tree.nodes) {
            out << std::string(node.depth * 2, ' ') << node.display_name
                << "  [" << node.logical_bytes << " B, " << node.file_count << " files]\n";
        }
        show_parity_report("Directory tree / sizes", out.str());
        labels();
        return true;
    }
    bool show_sync_directories() {
        if (!availability("ck.files.sync_directories").enabled)
            return false;
        const auto left_opt = pane_absolute_path(0);
        const auto right_opt = pane_absolute_path(1);
        if (!left_opt || !right_opt)
            return false;
        const auto &left_loc = *left_opt;
        const auto &right_loc = *right_opt;
        FilesSyncOptions opts;
        opts.compare_mode = FilesCompareMode::ThoroughContent;
        opts.direction = FilesSyncDirection::TwoWayCopyOnly;
        const auto plan = build_files_sync_plan(left_loc, right_loc, opts);
        std::ostringstream out;
        out << "Directory Compare & Reviewed Sync (Two-Way Copy-Only; no inferred deletions)\n"
            << "Left:  " << left_loc.display_text() << "\n"
            << "Right: " << right_loc.display_text() << "\n"
            << "Identical: " << plan.identical_count
            << " | Planned copies: " << plan.action_count
            << " | Conflicts requiring review: " << plan.conflict_count << "\n\n";
        if (!plan.error_message.empty())
            out << "Error: " << plan.error_message << "\n";
        const std::size_t shown = std::min<std::size_t>(plan.items.size(), 200);
        for (std::size_t idx = 0; idx < shown; ++idx) {
            const auto &it = plan.items[idx];
            if (it.relation == FilesCompareRelation::Identical)
                continue;
            const char *act = "None";
            switch (it.proposed_action) {
                case FilesSyncAction::CopyLeftToRight: act = "Copy L->R"; break;
                case FilesSyncAction::CopyRightToLeft: act = "Copy R->L"; break;
                case FilesSyncAction::CreateDirRight: act = "Mkdir R"; break;
                case FilesSyncAction::CreateDirLeft: act = "Mkdir L"; break;
                case FilesSyncAction::ReviewConflict: act = "CONFLICT (preserved)"; break;
                default: break;
            }
            out << "[" << act << "] " << it.relative_key;
            if (!it.diagnostic.empty())
                out << " — " << it.diagnostic;
            out << "\n";
        }
        show_parity_report("Synchronize directories (preview)", out.str());
        labels();
        return true;
    }
    bool show_diff_files() {
        if (!availability("ck.files.diff_files").enabled)
            return false;
        const auto left_file = selected_child_path(active);
        const auto right_file = selected_child_path(1 - active);
        if (!left_file || !right_file) {
            show_parity_report("Diff files", "Select a regular file in both panes to diff.");
            return false;
        }
        const auto diff = diff_native_files(*left_file, *right_file);
        std::ostringstream out;
        out << "Left:  " << left_file->display_text() << " (" << diff.left_size << " B)\n"
            << "Right: " << right_file->display_text() << " (" << diff.right_size << " B)\n\n";
        if (!diff.error_message.empty()) {
            out << "Error: " << diff.error_message << "\n";
        } else if (diff.identical) {
            out << "Files are byte-for-byte identical.\n";
        } else if (diff.binary) {
            out << "Binary diff (" << diff.binary_diffs.size() << " differing bytes shown):\n";
            for (const auto &bd : diff.binary_diffs) {
                out << "  0x" << std::hex << bd.offset << std::dec << ": ";
                if (bd.left_byte) out << std::hex << static_cast<unsigned>(*bd.left_byte) << std::dec;
                else out << "--";
                out << " vs ";
                if (bd.right_byte) out << std::hex << static_cast<unsigned>(*bd.right_byte) << std::dec;
                else out << "--";
                out << "\n";
            }
        } else {
            for (const auto &dl : diff.text_lines) {
                const char prefix = dl.kind == FilesDiffLineKind::Removed ? '-' :
                                    (dl.kind == FilesDiffLineKind::Added ? '+' : ' ');
                out << prefix << dl.text << "\n";
            }
        }
        show_parity_report("File diff", out.str());
        labels();
        return true;
    }
    bool show_archive_browse() {
        if (!availability("ck.files.archive_browse").enabled)
            return false;
        const auto arc = selected_child_path(active);
        if (!arc)
            return false;
        const auto cat = list_ordinary_archive(*arc);
        std::ostringstream out;
        out << "Archive: " << arc->display_text() << "\n";
        if (!cat.error_message.empty()) {
            out << "Error: " << cat.error_message << "\n";
        } else {
            out << "Entries: " << cat.entries.size()
                << " | Total uncompressed: " << cat.total_uncompressed_bytes << " B\n";
            if (cat.has_unsafe_entries)
                out << "WARNING: Archive contains unsafe paths/links (blocked on extract)\n";
            if (cat.has_collisions)
                out << "WARNING: Archive contains colliding member names\n";
            out << "\n";
            const std::size_t shown = std::min<std::size_t>(cat.entries.size(), 250);
            for (std::size_t idx = 0; idx < shown; ++idx) {
                const auto &e = cat.entries[idx];
                out << (e.kind == OrdinaryArchiveEntryKind::Directory ? "[DIR]  " : "[FILE] ")
                    << e.path << " (" << e.uncompressed_size << " B)";
                if (!e.safe_path)
                    out << " [UNSAFE: " << e.safety_warning << "]";
                out << "\n";
            }
        }
        show_parity_report("Archive browser", out.str());
        labels();
        return true;
    }
    bool show_archive_create() {
        if (!availability("ck.files.archive_create").enabled)
            return false;
        const auto src = selected_child_path(active);
        if (!src)
            return false;
        const auto dst_opt = pane_absolute_path(1 - active);
        if (!dst_opt)
            return false;
        const auto &dst_dir = *dst_opt;
        const auto split = detail::split_native_path(*src);
        if (!split)
            return false;
        const std::string arc_name_str = split->leaf.display_name() + ".zip";
        const auto arc_nn = detail::native_name_from_utf8(dst_dir.encoding(), arc_name_str);
        if (!arc_nn)
            return false;
        const auto arc_path = dst_dir.child_address(*arc_nn);
        if (!arc_path.value())
            return false;
        OrdinaryArchiveInputItem input;
        input.archive_path = split->leaf.display_name();
        input.source_path = *src;
        input.is_directory = (panes[active].model.cursor_kind() == ck::files::ObjectKind::Directory);
        const std::array<OrdinaryArchiveInputItem, 1> items{input};
        const auto res = create_ordinary_archive(*arc_path.value(), OrdinaryArchiveFormat::Zip, items);
        if (res.ok)
            refresh(1 - active);
        std::ostringstream out;
        out << "Create archive: " << arc_path.value()->display_text() << "\n"
            << (res.ok ? "Status: Created successfully\n" : ("Error: " + res.error_message + "\n"))
            << "Entries written: " << res.entries_written << " (" << res.archive_bytes << " B)\n";
        show_parity_report("Create archive", out.str());
        labels();
        return res.ok;
    }
    bool show_archive_extract() {
        if (!availability("ck.files.archive_extract").enabled)
            return false;
        const auto arc = selected_child_path(active);
        if (!arc)
            return false;
        const auto dst_opt = pane_absolute_path(1 - active);
        if (!dst_opt)
            return false;
        const auto &dst_dir = *dst_opt;
        const auto res = extract_ordinary_archive(*arc, dst_dir);
        if (res.extracted_files > 0 || res.extracted_directories > 0)
            refresh(1 - active);
        std::ostringstream out;
        out << "Extract archive: " << arc->display_text() << "\n"
            << "Destination: " << dst_dir.display_text() << "\n"
            << "Extracted files: " << res.extracted_files
            << " | Directories: " << res.extracted_directories
            << " | Conflicts skipped: " << res.skipped_conflicts
            << " | Unsafe rejected: " << res.rejected_unsafe << "\n";
        if (!res.error_message.empty())
            out << "Error: " << res.error_message << "\n";
        show_parity_report("Extract archive", out.str());
        labels();
        return res.error_message.empty();
    }
    void prune_embedded_editors() {
        embedded_editors.erase(
            std::remove_if(embedded_editors.begin(), embedded_editors.end(),
                           [](const EmbeddedEditorInstance &ed) {
                               return !ed.window || ed.lifetime.expired();
                           }),
            embedded_editors.end());
    }
    bool has_dirty_embedded_editors() const {
        for (const auto &ed : embedded_editors) {
            if (ed.window && !ed.lifetime.expired() && ed.document && ed.document->modified())
                return true;
        }
        return false;
    }
    bool save_embedded_editor(std::size_t index, bool force_overwrite) {
        prune_embedded_editors();
        if (index >= embedded_editors.size())
            return false;
        auto &ed = embedded_editors[index];
        if (!ed.document)
            return false;
        const auto saved = save_files_editor_document(
            ed.snapshot, ed.document->text(), force_overwrite);
        if (saved.status == FilesSaveStatus::Saved) {
            ed.snapshot = saved.updated_snapshot;
            ed.document->mark_clean();
            ++external_writer_epoch;
            refresh(active);
            labels();
            return true;
        }
        show_parity_report("Save failed", saved.diagnostic);
        labels();
        return false;
    }
    bool show_embedded_editor(bool new_document) {
        if (!availability(new_document ? "ck.files.new_edit" : "ck.files.edit").enabled)
            return false;
        prune_embedded_editors();
        if (!syntax_registry)
            syntax_registry = make_files_syntax_registry();
        auto doc = std::make_shared<ckv::widgets::EditorDocument>();
        FilesDocumentSnapshot snap;
        std::string title = "Untitled";
        if (!new_document) {
            const auto file_path = selected_child_path(active);
            if (!file_path)
                return false;
            const auto opened = open_files_editor_document(*file_path);
            if (!opened.ok) {
                show_parity_report("Editor", "Unable to open file: " + opened.error_message);
                return false;
            }
            doc->set_text(opened.text);
            doc->mark_clean();
            snap = opened.snapshot;
            const auto split = detail::split_native_path(*file_path);
            title = split ? split->leaf.display_name() : file_path->display_text();
        } else {
            doc->mark_clean();
        }
        using namespace ckv::widgets;
        auto win = std::make_unique<Window>("Edit: " + title);
        win->set_command_context("ck.files.editor");
        auto ed = std::make_unique<TextEditor>(doc, syntax_registry.get());
        ed->set_command_context("ck.files.editor");
        ed->set_file_name(title);
        ed->set_show_line_numbers(true);
        auto *raw_ed = ed.get();
        win->set_content(std::move(ed));
        const auto desk = shell->desktop().bounds();
        const int w = std::max(50, std::min(100, desk.width - 4));
        const int h = std::max(14, std::min(30, desk.height - 4));
        win->set_bounds({(desk.width - w) / 2, (desk.height - h) / 2, w, h});

        Window *raw_win = win.get();
        auto win_life = win->lifetime_token();
        win->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
            if (weak.expired())
                return;
            prune_embedded_editors();
            if (embedded_editors.empty() && surfaces[active] && surfaces[active]->grid)
                application.set_focus(surfaces[active]->grid);
            sync_active_view_chrome();
            labels();
        };
        win->close_request = [this, weak = std::weak_ptr<void>(lifetime), raw_win, win_life, title]() -> bool {
            if (weak.expired() || win_life.expired())
                return true;
            auto it = std::find_if(embedded_editors.begin(), embedded_editors.end(),
                                   [raw_win](const EmbeddedEditorInstance &e) { return e.window == raw_win; });
            if (it == embedded_editors.end() || it->allow_discard_close ||
                !it->document || !it->document->modified())
                return true;
            if (parity_dialog && !parity_dialog->completed())
                return false;
            DialogDescriptor desc;
            desc.title = "Unsaved changes: " + title;
            desc.fields = {FieldDescriptor{.label = "Document has unsaved edits. Discard changes and close?",
                                           .kind = FieldKind::Note}};
            desc.buttons = {
                {"&Stay", ButtonRole::Dismiss, {}},
                {"&Discard and close", ButtonRole::Accept, {}}};
            parity_dialog.emplace(present_modal_dialog(std::move(desc), application, shell->desktop()));
            parity_dialog->set_completion_handler(
                [this, weak, raw_win, win_life](const DialogResult &res) {
                if (weak.expired())
                    return;
                parity_dialog.reset();
                if (res.accepted && !win_life.expired()) {
                    for (auto &e : embedded_editors) {
                        if (e.window == raw_win) {
                            e.allow_discard_close = true;
                            break;
                        }
                    }
                    raw_win->close();
                }
                labels();
            });
            return false; // Veto close until user confirms.
        };

        EmbeddedEditorInstance inst;
        inst.window = raw_win;
        inst.editor = raw_ed;
        inst.lifetime = win_life;
        inst.document = doc;
        inst.snapshot = std::move(snap);
        embedded_editors.push_back(std::move(inst));
        shell->desktop().add_window(std::move(win));
        application.set_focus(raw_ed);
        sync_active_view_chrome();
        labels();
        return true;
    }
    bool show_embedded_terminal() {
        if (!availability("ck.files.terminal").enabled)
            return false;
        using namespace ckv::widgets;
        if (embedded_terminal_window && !embedded_terminal_lifetime.expired()) {
            if (auto *stop = ckv::ui::Application::first_focus_stop(*embedded_terminal_window))
                application.set_focus(stop);
            return true;
        }
        const auto cwd_opt = pane_absolute_path(active);
        const std::optional<std::string> cwd_host = cwd_opt ? detail::host_path_utf8(*cwd_opt) : std::nullopt;
        const std::string cwd = cwd_host
            ? *cwd_host
            : (cwd_opt ? cwd_opt->display_text() : std::string("/"));
        auto spec = build_files_interactive_terminal_spec(cwd);
        auto &subsession = application.launch_terminal_subsession(std::move(spec));
        auto win = std::make_unique<Window>("Terminal: " + cwd);
        win->set_command_context("ck.files.terminal");
        auto tview = std::make_unique<TerminalView>(subsession);
        tview->set_command_context("ck.files.terminal");
        auto *raw_tview = tview.get();
        tview->on_parent_escape = [this, weak = std::weak_ptr<void>(lifetime)] {
            if (weak.expired())
                return;
            if (surfaces[active] && surfaces[active]->grid)
                application.set_focus(surfaces[active]->grid);
            sync_active_view_chrome();
            ++external_writer_epoch;
            refresh(active);
        };
        win->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
            if (weak.expired())
                return;
            embedded_terminal_window = nullptr;
            if (surfaces[active] && surfaces[active]->grid)
                application.set_focus(surfaces[active]->grid);
            sync_active_view_chrome();
            labels();
        };
        win->set_content(std::move(tview));
        const auto desk = shell->desktop().bounds();
        const int w = std::max(50, std::min(100, desk.width - 4));
        const int h = std::max(14, std::min(28, desk.height - 4));
        win->set_bounds({(desk.width - w) / 2, (desk.height - h) / 2, w, h});
        embedded_terminal_window = win.get();
        embedded_terminal_lifetime = win->lifetime_token();
        ++external_writer_epoch;
        shell->desktop().add_window(std::move(win));
        application.set_focus(raw_tview);
        sync_active_view_chrome();
        labels();
        return true;
    }
    bool show_run_command() {
        if (!availability("ck.files.run_command").enabled)
            return false;
        ckv::widgets::DialogDescriptor desc;
        desc.title = "Run direct command (argv, no shell)";
        desc.fields = {ckv::widgets::FieldDescriptor{.label = "&Command (executable arg1 ...):"}};
        desc.buttons = {
            {"&Run", ckv::widgets::ButtonRole::Accept, {}},
            {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        const std::size_t pane_idx = active;
        const auto cwd_opt = pane_absolute_path(pane_idx);
        const std::optional<std::string> cwd_host = cwd_opt ? detail::host_path_utf8(*cwd_opt) : std::nullopt;
        const std::string cwd = cwd_host
            ? *cwd_host
            : (cwd_opt ? cwd_opt->display_text() : std::string("/"));
        parity_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(desc), application, shell->desktop()));
        parity_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane_idx, cwd](const ckv::widgets::DialogResult &res) {
            if (weak.expired())
                return;
            std::string cmd;
            if (res.accepted && !res.values.empty())
                cmd = res.values[0];
            parity_dialog.reset();
            if (res.accepted && !cmd.empty()) {
                command_history.record(cmd);
                const auto expanded = parse_files_direct_command(cmd, cwd);
                const auto run_res = execute_files_command(expanded, application.clock());
                ++external_writer_epoch;
                refresh(pane_idx);
                std::ostringstream out;
                out << "Command: " << expanded.review_preview << "\n"
                    << "Success: " << (run_res.successful() ? "yes" : "no") << "\n";
                if (!run_res.diagnostic.empty())
                    out << "Diagnostic: " << run_res.diagnostic << "\n";
                if (!run_res.stdout_capture.bytes.empty())
                    out << "\n--- stdout ---\n" << run_res.stdout_capture.bytes << "\n";
                if (!run_res.stderr_capture.bytes.empty())
                    out << "\n--- stderr ---\n" << run_res.stderr_capture.bytes << "\n";
                show_parity_report("Command output", out.str());
            }
            labels();
        });
        return true;
    }
    bool show_user_menu() {
        if (!availability("ck.files.user_menu").enabled)
            return false;
        const auto menu = default_files_user_menu();
        FilesCommandPlaceholderContext ctx;
        if (const auto ap = pane_absolute_path(active))
            ctx.active_directory = ap->display_text();
        if (const auto op = pane_absolute_path(1 - active))
            ctx.opposite_directory = op->display_text();
        if (const auto sel = selected_child_path(active)) {
            ctx.current_path = sel->display_text();
            if (const auto sp = detail::split_native_path(*sel))
                ctx.current_name = sp->leaf.display_name();
        }
        std::ostringstream out;
        out << "User Menu (reviewed placeholder previews — direct argv safe):\n\n";
        for (std::size_t idx = 0; idx < menu.size(); ++idx) {
            const auto exp = expand_files_user_menu_entry(menu[idx], ctx);
            out << (idx + 1) << ". " << menu[idx].label << "\n   "
                << (exp.valid ? exp.review_preview : ("(unavailable: " + exp.error_message + ")"))
                << "\n\n";
        }
        show_parity_report("User menu", out.str());
        labels();
        return true;
    }
    bool show_connect_remote() {
        if (!availability("ck.files.connect_remote").enabled)
            return false;
        ckv::widgets::DialogDescriptor desc;
        desc.title = "Connect remote location (SFTP / SMB)";
        desc.fields = {ckv::widgets::FieldDescriptor{.label = "&URI (sftp://user@host/path or smb://host/share):"}};
        desc.buttons = {
            {"&Connect", ckv::widgets::ButtonRole::Accept, {}},
            {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        parity_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(desc), application, shell->desktop()));
        parity_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime)](const ckv::widgets::DialogResult &res) {
            if (weak.expired())
                return;
            std::string raw_uri;
            if (res.accepted && !res.values.empty())
                raw_uri = res.values[0];
            parity_dialog.reset();
            if (res.accepted && !raw_uri.empty()) {
                const auto parsed = parse_files_remote_uri(raw_uri);
                std::ostringstream out;
                out << "Target: " << parsed.redacted_display() << "\n";
                if (!parsed.valid) {
                    out << "Error: " << parsed.error_message << "\n";
                } else {
                    const auto caps = provider_capabilities_for(parsed.kind);
                    out << "Provider: " << caps.transport_summary << "\n";
                    std::unique_ptr<FilesRemoteTransport> transport =
                        parsed.kind == FilesProviderKind::Sftp
                            ? make_sftp_remote_transport(application.clock())
                            : make_smb_mounted_transport();
                    FilesRemoteSession session(std::move(transport), FilesHostKeyPolicy::StrictVerify);
                    const auto listed = session.list(parsed);
                    if (listed.status == FilesRemoteStatus::Ok) {
                        out << "Entries: " << listed.entries.size() << "\n\n";
                        for (const auto &e : listed.entries)
                            out << (e.is_directory ? "[DIR]  " : "[FILE] ") << e.name
                                << " (" << e.size << " B)\n";
                    } else {
                        out << "Status: " << redact_remote_secrets(listed.diagnostic) << "\n";
                    }
                }
                show_parity_report("Remote location", out.str());
            }
            labels();
        });
        return true;
    }
    bool show_inspect_volume_archive() {
        if (!availability("ck.files.inspect_volume_archive").enabled)
            return false;
        const auto arc = selected_child_path(active);
        if (!arc)
            return false;
        const auto info = inspect_volume_archive_file(*arc);
        std::ostringstream out;
        out << "CKARV01 Volume Archive Inspection: " << arc->display_text() << "\n";
        if (!info.valid) {
            out << "Error: " << info.error_message << "\n";
        } else {
            out << "Format version: " << info.format_version
                << " | Source: " << info.source.name << " (" << info.source.filesystem << ")\n"
                << "Entries: " << info.entries.size()
                << " (dirs=" << info.directory_count
                << ", files=" << info.regular_file_count
                << ", sparse=" << info.sparse_file_count
                << ", links=" << info.symlink_count
                << ", hardlinks=" << info.hard_link_count << ")\n"
                << "Recorded losses: " << info.losses.size() << "\n";
        }
        show_parity_report("Volume archive inspection", out.str());
        labels();
        return info.valid;
    }
    bool show_restore_volume_archive() {
        if (!availability("ck.files.restore_volume_archive").enabled)
            return false;
        const auto arc = selected_child_path(active);
        if (!arc)
            return false;
        const auto dst_opt = pane_absolute_path(1 - active);
        if (!dst_opt)
            return false;
        const auto &dst_dir = *dst_opt;
        const auto plan = plan_volume_archive_restore(*arc, dst_dir);
        std::ostringstream out;
        out << "CKARV01 Volume Archive Verified Restore\n"
            << "Archive: " << arc->display_text() << "\n"
            << "Destination: " << dst_dir.display_text() << "\n";
        if (!plan.valid) {
            out << "Refused: " << plan.error_message << "\n";
            show_parity_report("Volume archive restore", out.str());
            return false;
        }
        const auto res = restore_volume_archive(plan, false);
        if (res.restored_files > 0 || res.restored_directories > 0)
            refresh(1 - active);
        out << "Status: " << (res.ok ? "Verified restore complete" : ("Failed: " + res.error_message)) << "\n"
            << "Restored files: " << res.restored_files
            << " (sparse=" << res.restored_sparse_files << ")"
            << " | Dirs: " << res.restored_directories
            << " | Hard links: " << res.restored_hard_links
            << " | Symlinks: " << res.restored_symlinks
            << " | Conflicts skipped: " << res.skipped_conflicts
            << " | Verified bytes: " << res.verified_bytes << "\n";
        show_parity_report("Volume archive restore", out.str());
        labels();
        return res.ok;
    }
    bool change_view(std::size_t i, ck::files::PaneView view) {
        if (!ready(i) || !ck::files::valid_pane_order({view.sort, view.descending, view.folders_first}) ||
            view.display_substring.size() > 4096)
            return false;
        if (auto prepared = prepare_current_row(i)) {
            if (*prepared)
                panes[i].queued_view = std::move(view);
            labels();
            return *prepared;
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
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
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
    void show_create_file() {
        if (!ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto encoding = panes[pane].encoding;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Create empty file";
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
        create_file_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        create_file_window = shell->desktop().active_window();
        create_file_lifetime = create_file_window->lifetime_token();
        create_file_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, encoding](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                create_file_window = nullptr;
                if (!result.accepted) {
                    create_file_dialog.reset();
                    return;
                }
                if (result.values.size() != 2 || panes[pane].navigation != navigation) {
                    panes[pane].error = "Location changed while file creation was open";
                    create_file_dialog.reset();
                    labels();
                    return;
                }
                auto name = parse_directory_component(result.values[0], encoding);
                create_file_dialog.reset();
                if (!name || !create_file(pane, std::move(*name))) {
                    panes[pane].error = "File name is not representable or the location changed";
                    labels();
                }
            });
    }
    void show_create_hard_link() {
        if (!ready(active) || panes[active].model.cursor_kind() != ck::files::ObjectKind::RegularFile)
            return;
        const auto pane = active;
        const auto source_target = panes[pane].model.target();
        if (!source_target)
            return;
        const auto navigation = panes[pane].navigation;
        const auto encoding = panes[pane].encoding;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Create hard link";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "&Name:",
                .validate = [encoding](const std::string &value) {
                    return parse_directory_component(value, encoding).has_value();
                },
                .description = encoding == ck::files::NameEncoding::WindowsUtf16
                                   ? "Strict UTF-8 input; the destination is created in the current directory without replacement."
                                   : "One exact native component; the destination is created without replacement."},
            ckv::widgets::FieldDescriptor{.label = "The selected regular file remains the same object; only a new name is created.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Create", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        create_hard_link_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        create_hard_link_window = shell->desktop().active_window();
        create_hard_link_lifetime = create_hard_link_window->lifetime_token();
        create_hard_link_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, encoding, source = *source_target](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                create_hard_link_window = nullptr;
                if (!result.accepted) {
                    create_hard_link_dialog.reset();
                    return;
                }
                if (result.values.size() != 2 || panes[pane].navigation != navigation) {
                    panes[pane].error = "Location changed while hard-link creation was open";
                    create_hard_link_dialog.reset();
                    labels();
                    return;
                }
                auto name = parse_directory_component(result.values[0], encoding);
                create_hard_link_dialog.reset();
                if (!name || !create_hard_link(pane, source, std::move(*name))) {
                    panes[pane].error = "Hard-link name is not representable or the location changed";
                    labels();
                }
            });
    }
    void show_create_symbolic_link() {
        if (!availability("ck.files.create_symbolic_link").enabled || !ready(active))
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        const auto encoding = panes[pane].encoding;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Create symbolic link";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "&Name:",
                .validate = [encoding](const std::string &value) {
                    return parse_directory_component(value, encoding).has_value();
                },
                .description = "One exact native destination component; creation is exclusive in the current directory."},
            ckv::widgets::FieldDescriptor{
                .label = "&Target:",
                .validate = [encoding](const std::string &value) {
                    return parse_symbolic_link_target(value, encoding).has_value();
                },
                .description = "Opaque POSIX link payload. It is not normalized, opened, or followed."},
            ckv::widgets::FieldDescriptor{
                .label = "The target is stored as link data. A relative target remains relative to the new link.",
                .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Create", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        create_symbolic_link_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        create_symbolic_link_window = shell->desktop().active_window();
        create_symbolic_link_lifetime = create_symbolic_link_window->lifetime_token();
        create_symbolic_link_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, encoding](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                create_symbolic_link_window = nullptr;
                if (!result.accepted) {
                    create_symbolic_link_dialog.reset();
                    return;
                }
                if (result.values.size() != 3 || panes[pane].navigation != navigation) {
                    panes[pane].error = "Location changed while symbolic-link creation was open";
                    create_symbolic_link_dialog.reset();
                    labels();
                    return;
                }
                auto name = parse_directory_component(result.values[0], encoding);
                auto target = parse_symbolic_link_target(result.values[1], encoding);
                create_symbolic_link_dialog.reset();
                if (!name || !target || !create_symbolic_link(pane, std::move(*target), std::move(*name))) {
                    panes[pane].error = "Symbolic-link name or target is not representable, or the location changed";
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
        if (!availability("ck.files.copy").enabled)
            return;
        const auto volume = panes[active].model.volume_cursor();
        const auto destination_pane = 1 - active;
        const auto destination_location = panes[destination_pane].active;
        if (!volume || !volume->archive.root || !destination_location)
            return;
        const auto destination = native_location_address(*destination_location);
        if (!destination) {
            panes[active].error = "Volume archive unavailable: destination address is not representable";
            labels();
            return;
        }
        VolumeArchiveOrigin origin{active, panes[active].navigation, panes[destination_pane].navigation,
                                   *volume, destination_location};
        auto capture_source = [source = origin.volume, scratch = volume_archive_scratch_root](std::stop_token token) {
            return capture_native_volume_archive_source(source, *scratch, FilesVolumeArchiveConsistency::Live, token);
        };
        FilesVolumeArchivePlanningRequest request{
            origin.volume, *destination, FilesVolumeArchiveReview::default_archive_name(origin.volume.name),
            FilesVolumeArchiveConsistency::Live, std::move(capture_source)};
        const auto submitted = volume_archive_service->plan(std::move(request));
        if (submitted.state != FilesVolumeArchivePlanningSubmission::Accepted) {
            panes[active].error = "Volume archive planning worker is unavailable";
            labels();
            return;
        }
        volume_archive_origin = std::move(origin);
        volume_archive_planning_request = submitted.ticket;
        volume_archive_planning_cancelling = false;
        panes[active].error.clear();
        panes[destination_pane].archive_notice.clear();
        watch();
        labels();
    }
    bool start_transfer(FilesOperationExecutionRequest execution, std::size_t source, std::size_t destination) {
        const OperationExecutionOrigin origin{execution.operation, source,
            execution.source_navigation, execution.destination_navigation};
        execution.open_journal = [](const ck::files::NativeAbsolutePath &root,
                                    const ck::files::NativeName &basename, bool create_new) {
            return ck::files::open_native_operation_journal_file(root, basename, create_new);
        };
        const auto submitted = execution_service->request(std::move(execution));
        if (submitted.state != FilesOperationExecutionSubmission::Accepted) {
            panes[source].error = origin.kind == ck::files::FileOperation::Move
                ? "Move worker is unavailable" : "Copy worker is unavailable";
            labels();
            return false;
        }
        operation_execution_request = submitted.ticket;
        operation_execution_origin = origin;
        panes[source].error.clear();
        panes[destination].error.clear();
        watch();
        labels();
        return true;
    }

    void prepare_copy(ck::files::FileOperation operation) {
        if (operation != ck::files::FileOperation::Copy && operation != ck::files::FileOperation::Move)
            return;
        const auto command = operation == ck::files::FileOperation::Copy ? "ck.files.copy" : "ck.files.move";
        if (!availability(command).enabled)
            return;
        if (operation == ck::files::FileOperation::Copy && panes[active].volume_view) {
            prepare_volume_archive();
            return;
        }
        const auto source_pane = active;
        const auto destination_pane = 1 - active;
        if (panes[source_pane].selection.origin != ck::files::PaneSourcesOrigin::Marks) {
            if (auto prepared = prepare_current_row(source_pane)) {
                if (*prepared)
                    panes[source_pane].queued_copy = operation;
                labels();
                return;
            }
        }
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
        // Bound retained handles during traversal and size the downstream
        // evidence stages for the same scope. Execution reuses these limits.
        const auto max_items = planning.source_limits.held_entries;
        planning.evidence_limits.max_items = max_items;
        planning.evidence_limits.recursive.inventory.max_entries = max_items;
        planning.evidence_limits.destination_batch.rows = max_items;
        planning.evidence_limits.source_target.rows = max_items;
        planning.projection_policy.require_unhandled_metadata_consent = true;
        planning.journal_root = *operation_journal_root;
        const auto revalidate = [this, source_pane, destination_pane, source_navigation, destination_navigation](
                                    const ck::files::ConsentReceipt &receipt) {
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
                                 source_stamp, destination_stamp, source_limits = planning.source_limits](
                                    ck::files::Observation<ck::files::ConsentReceipt> result,
                                    FilesOperationReviewEvidence::Pointer evidence) {
            if (!result.value()) {
                const auto *failure = result.unavailable_detail();
                if (failure && failure->error.domain == ck::files::ErrorDomain::Posix &&
                    (failure->error.code == EMFILE || failure->error.code == ENFILE)) {
                    panes[source_pane].error = "Open-file limit reached during planning";
                    labels();
                    return;
                }
                panes[source_pane].error = std::string(operation == ck::files::FileOperation::Move ? "Move" : "Copy") +
                    " was not authorized: " +
                    (failure ? failure->error.operation : "review failed");
                labels();
                return;
            }
            if (result.value()->action == ck::files::ConsentAction::ChooseAnotherDestination) {
                if (panes[destination_pane].navigation == destination_navigation)
                    show_location(destination_pane);
                return;
            }
            if (result.value()->action == ck::files::ConsentAction::Cancel) {
                labels();
                return;
            }
            FilesOperationExecutionRequest execution{
                operation, services[source_pane], services[destination_pane], source_navigation, destination_navigation,
                source_stamp, destination_stamp, source_limits, *operation_journal_root, {}, {}, {},
                std::move(evidence), std::move(result).take_value()};
            start_transfer(std::move(execution), source_pane, destination_pane);
        };
        if (!submit_operation_planning(std::move(planning), panes[destination_pane].encoding,
                                      revalidate, completion, true)) {
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
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
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
        const std::size_t marked = panes[pane].selection.marks ? panes[pane].selection.marks->total : std::size_t{0};
        const auto target = panes[pane].model.target();
        if (marked == 0 && !target)
            return;
        const auto row = target ? target->row : ck::files::PaneRowId{};
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = marked > 0
            ? "Move " + std::to_string(marked) + " marked entries to Trash / Recycle Bin"
            : "Move entry to Trash / Recycle Bin";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "The platform Trash / Recycle Bin will receive the selected entry when supported. No permanent-delete fallback is used.",
                .kind = ckv::widgets::FieldKind::Note},
            ckv::widgets::FieldDescriptor{.label = "The worker will revalidate the selected entry identity before moving it.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Move to Trash", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        trash_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        trash_window = shell->desktop().active_window();
        trash_lifetime = trash_window->lifetime_token();
        trash_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, row, marked](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                trash_window = nullptr;
                if (!result.accepted) {
                    trash_dialog.reset();
                    return;
                }
                if (marked > 0) {
                    if (panes[pane].navigation != navigation) {
                        panes[pane].error = "Folder changed while Trash / Recycle Bin was open";
                        trash_dialog.reset();
                        labels();
                        return;
                    }
                    trash_dialog.reset();
                    if (!start_batch_delete(pane, Pane::BatchDeleteKind::Trash)) {
                        panes[pane].error = "Batch Trash / Recycle Bin request was rejected";
                        labels();
                    }
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
        const std::size_t marked = panes[pane].selection.marks ? panes[pane].selection.marks->total : std::size_t{0};
        const auto target = panes[pane].model.target();
        if (marked == 0 && !target)
            return;
        const auto row = target ? target->row : ck::files::PaneRowId{};
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = marked > 0
            ? "Delete " + std::to_string(marked) + " marked entries permanently"
            : "Delete entry permanently";
        descriptor.fields = {
            ckv::widgets::FieldDescriptor{
                .label = "This is irreversible. The selected entry will be permanently deleted; it will not be moved to Trash or the Recycle Bin.",
                .kind = ckv::widgets::FieldKind::Note},
            ckv::widgets::FieldDescriptor{.label = "The worker will revalidate the selected entry identity before deletion.",
                                          .kind = ckv::widgets::FieldKind::Note}};
        descriptor.buttons = {{"&Delete permanently", ckv::widgets::ButtonRole::Accept, {}},
                              {"&Cancel", ckv::widgets::ButtonRole::Dismiss, {}}};
        delete_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        delete_window = shell->desktop().active_window();
        delete_lifetime = delete_window->lifetime_token();
        delete_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation, row, marked](
                const ckv::widgets::DialogResult &result) {
                if (weak.expired())
                    return;
                delete_window = nullptr;
                if (!result.accepted) {
                    delete_dialog.reset();
                    return;
                }
                if (marked > 0) {
                    if (panes[pane].navigation != navigation) {
                        panes[pane].error = "Folder changed while permanent deletion was open";
                        delete_dialog.reset();
                        labels();
                        return;
                    }
                    delete_dialog.reset();
                    if (!start_batch_delete(pane, Pane::BatchDeleteKind::PermanentDelete)) {
                        panes[pane].error = "Batch permanent deletion request was rejected";
                        labels();
                    }
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
        show_location(active);
    }
    void show_location(std::size_t pane) {
        if (!can_edit_location(pane))
            return;
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
                if (!panes[pane].navigating && !panes[pane].volume_view && !volume_pending(pane) &&
                    panes[pane].active && panes[pane].model.current() &&
                    *panes[pane].active == *location)
                    return;
                auto view = panes[pane].view;
                view.display_substring.clear();
                navigate(pane, std::move(*location), std::move(view));
            });
    }
    void show_bookmarks() {
        if (!availability("ck.files.bookmark_open").enabled)
            return;
        const auto pane = active;
        const auto navigation = panes[pane].navigation;
        auto bookmarks = panes[pane].bookmarks;
        std::vector<std::string> names;
        names.reserve(bookmarks.size());
        for (const auto &bookmark : bookmarks)
            names.push_back(bookmark->display_text());

        using namespace ckv::widgets;
        DialogDescriptor descriptor;
        descriptor.title = pane == 0 ? "Left bookmarks" : "Right bookmarks";
        descriptor.fields = {{.label = "&Location", .kind = FieldKind::Combo,
                              .options = std::move(names), .initial_selection = 0}};
        descriptor.buttons = {{"&Open", ButtonRole::Accept, {}}, {"&Cancel", ButtonRole::Dismiss, {}}};
        bookmark_dialog.emplace(
            present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        bookmark_dialog_window = shell->desktop().active_window();
        bookmark_dialog_lifetime = bookmark_dialog_window->lifetime_token();
        bookmark_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime), pane, navigation,
             snapshot = std::move(bookmarks)](const DialogResult &result) {
                if (weak.expired())
                    return;
                bookmark_dialog_window = nullptr;
                if (close_after_bookmark) {
                    close_after_bookmark = false;
                    request_close();
                    return;
                }
                if (!result.accepted || result.selected.size() != 1 || result.selected[0] < 0)
                    return;
                const auto index = static_cast<std::size_t>(result.selected[0]);
                if (panes[pane].navigation != navigation || !ready(pane)) {
                    panes[pane].error = "Location changed while the bookmark picker was open";
                    labels();
                    return;
                }
                const auto &current = panes[pane].bookmarks;
                if (index >= snapshot.size() || index >= current.size() || snapshot[index] != current[index]) {
                    panes[pane].error = "The selected bookmark changed";
                    labels();
                    return;
                }
                navigate_bookmark(pane, index);
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
        view_dialog.emplace(present_modal_dialog(std::move(descriptor), application, shell->desktop()));
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
    bool properties_open() const {
        return properties_dialog_window && !properties_dialog_lifetime.expired();
    }
    void show_properties() {
        if (!availability("ck.files.properties").enabled)
            return;
        const auto present = [this](std::string title, std::string text) {
            properties_dialog_window = present_files_read_only_text(std::move(title), std::move(text),
                application, shell->desktop(), shell->roles());
            if (properties_dialog_window) {
                properties_dialog_lifetime = properties_dialog_window->lifetime_token();
                properties_dialog_window->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
                    if (!weak.expired()) {
                        properties_dialog_window = nullptr;
                        labels();
                    }
                };
                labels();
            }
        };
        const auto pane = active;
        if (panes[pane].volume_view) {
            const auto volume = panes[pane].model.volume_cursor();
            if (!volume)
                return;
            const auto exact = [](const auto &value) {
                return value ? std::to_string(*value) + " bytes" : std::string("Unknown");
            };
            std::string text;
            if (panes[pane].volume_inventory_stale)
                text = "Last successful scan. Refresh to update these values.\n\n";
            if (!volume->name.empty())
                text += "Volume: " + ck::files::display_native_text(volume->name) + '\n';
            text += "Mount: " + volume->display_mount() + "\nFilesystem: " +
                (volume->filesystem.empty() ? "Unknown" : ck::files::display_native_text(volume->filesystem)) +
                "\nAccess: " + (!volume->read_only ? "Unknown" : *volume->read_only ? "Read-only" : "Read/write") +
                "\nAvailable: " + exact(volume->free_bytes) + "\nTotal: " + exact(volume->total_bytes);
            present("Volume properties", std::move(text));
            return;
        }
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
        present("Properties", "Name: " + display_name + "\nLocation: " + location + "\nType: " + kind +
            "\nLogical bytes: " + number(row->metadata.logical_size) +
            "\nAllocated bytes: " + number(row->metadata.allocated_size) +
            "\nLinks: " + number(row->metadata.link_count) + "\nModified: " + timestamp(row->metadata.modified));
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
        panes[pane].inspection = RowInspection{submitted.ticket, FilesPaneReplyKind::Preview};
        watch();
        labels();
    }
    void show_inspect_link() {
        if (!availability("ck.files.inspect_link").enabled)
            return;
        const auto pane = active;
        const auto target = panes[pane].model.target();
        if (!target)
            return;
        const auto submitted = services[pane]->inspect_link(panes[pane].navigation, target->stamp, target->row);
        if (submitted.state != FilesPaneSubmission::Accepted) {
            panes[pane].error = "Link inspection request rejected";
            labels();
            return;
        }
        panes[pane].inspection = RowInspection{submitted.ticket, FilesPaneReplyKind::Link};
        labels();
    }
    void copy_path() {
        if (!availability("ck.files.copy_path").enabled)
            return;
        auto &pane = panes[active];
        const auto target = pane.model.target();
        if (!target)
            return;
        const auto submitted = services[active]->copy_path(pane.navigation, target->stamp, target->row);
        if (submitted.state == FilesPaneSubmission::Accepted) {
            pane.inspection = RowInspection{submitted.ticket, FilesPaneReplyKind::PathText};
            watch();
        } else {
            pane.error = "Copy path request rejected";
        }
        labels();
    }
    void watch() {
        const bool pending = settings_waiting || operation_planning_request.has_value() || volume_request ||
            operation_execution_request.has_value() ||
            volume_archive_recovery_request ||
            volume_archive_planning_request.has_value() || volume_archive_execution_request.has_value() ||
            std::any_of(panes.begin(), panes.end(), [](const Pane &p) {
                return State::pending(p) || p.archive_refresh.has_value();
            });
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
        p.cancelling = false;
        if (reply.state != FilesPaneReplyState::Ready || !reply.preview) {
            p.error = failure(reply);
            labels();
            return;
        }
        if (modal_pending()) {
            p.error = "Preview ready. Close the current dialog and open the file again.";
            labels();
            return;
        }
        using namespace ckv::widgets;
        const auto preview = std::move(reply.preview);
        auto text = preview_utf8(*preview);
        auto owner = std::make_unique<Window>(text ? "Text preview" : "Hex preview");
        owner->set_command_context(text ? "ck.files.preview.text" : "ck.files.preview.hex_only");
        auto *const view = owner.get();
        const auto close = [view] { view->close(); };
        const auto &roles = shell->roles();
        owner->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
        owner->set_content_margin(1, 1);
        const auto area = shell->desktop().content_area();
        const int width = std::min(90, area.width);
        const int height = std::min(22, area.height);
        owner->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
        owner->set_min_size({32, 8});
        owner->set_footer(preview->complete ? "Read-only" :
            "Read-only | First " + std::to_string(preview->bytes.size()) + " bytes");
        owner->cancel_request = close;
        owner->on_detached = [this, weak = std::weak_ptr<void>(lifetime)] {
            if (weak.expired())
                return;
            preview_dialog_window = nullptr;
            preview_toggle_action = {};
            preview_is_hex = false;
            preview_can_toggle = false;
            sync_active_view_chrome();
            labels();
        };
        auto content = std::make_unique<ckv::ui::Column>();
        auto reader = std::make_unique<TextView>();
        auto *const display = reader.get();
        reader->set_text(text ? *text : preview_hex(*preview));
        content->add_item(std::move(reader), {.policy = ckv::ui::SizePolicy::Expanding});
        auto buttons = std::make_unique<ckv::ui::Row>();
        buttons->set_spacing(1);
        preview_is_hex = !text.has_value();
        preview_can_toggle = text.has_value();
        preview_toggle_action = {};
        if (text) {
            auto toggle = std::make_unique<Button>("Show &hex");
            auto *const mode = toggle.get();
            auto do_toggle = std::make_shared<std::function<void()>>();
            *do_toggle = [this, weak = std::weak_ptr<void>(lifetime), view, display, mode,
                          decoded = std::move(*text), preview, hex = false]() mutable {
                if (weak.expired())
                    return;
                hex = !hex;
                preview_is_hex = hex;
                display->set_text(hex ? preview_hex(*preview) : decoded);
                display->set_top_line(0);
                mode->set_text(hex ? "Show &text" : "Show &hex");
                view->set_title(hex ? "Hex preview" : "Text preview");
                view->set_command_context(hex ? "ck.files.preview.hex" : "ck.files.preview.text");
                sync_active_view_chrome();
            };
            toggle->on_press = [do_toggle] { (*do_toggle)(); };
            preview_toggle_action = [do_toggle] { (*do_toggle)(); };
            buttons->add_item(std::move(toggle));
        }
        auto dismiss = std::make_unique<Button>("&Close");
        dismiss->on_press = close;
        buttons->add_item(std::move(dismiss));
        content->add_item(std::move(buttons));
        owner->set_content(std::move(content));
        preview_dialog_lifetime = view->lifetime_token();
        preview_dialog_window = shell->desktop().present_modal(WindowHandle{std::move(owner), display}, application);
        sync_active_view_chrome();
        labels();
    }
    bool preview_open() const { return preview_dialog_window && !preview_dialog_lifetime.expired(); }
    void consume_link(std::size_t i, FilesPaneReply reply) {
        auto &p = panes[i];
        p.cancelling = false;
        if (reply.state != FilesPaneReplyState::Ready || !reply.link) {
            p.error = failure(reply);
            labels();
            return;
        }
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Link target";
        descriptor.resizable = true;
        descriptor.fields = {
            {.label = "Read-only metadata from the held link/reparse object. The target was not followed or executed.",
             .kind = ckv::widgets::FieldKind::Note},
            {.label = "Metadata", .initial_text = link_text(*reply.link), .kind = ckv::widgets::FieldKind::Memo},
        };
        descriptor.buttons = {{"&Close", ckv::widgets::ButtonRole::Dismiss, {}}};
        link_dialog.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        link_dialog_window = shell->desktop().active_window();
        link_dialog_lifetime = link_dialog_window->lifetime_token();
        link_dialog->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime)](const ckv::widgets::DialogResult &) {
                if (!weak.expired())
                    link_dialog_window = nullptr;
            });
        labels();
    }
    void consume_path_text(std::size_t i, FilesPaneReply reply) {
        auto &pane = panes[i];
        if (std::exchange(pane.cancelling, false)) {
            pane.error = "Copy path cancelled";
        } else if (reply.state != FilesPaneReplyState::Ready || !reply.path_text) {
            pane.error = failure(reply);
        } else if (!closing) {
            const std::string subject = reply.path_text->exact_names ? "Exact Names address" : "Path";
            const auto result = application.set_clipboard_text(std::move(reply.path_text->text));
            if (result.status == ckv::ClipboardWriteStatus::Ok)
                pane.error = subject + " copied to clipboard";
            else if (result.status == ckv::ClipboardWriteStatus::Submitted)
                pane.error = subject + " copied in Files; clipboard request sent to terminal";
            else
                pane.error = subject + " copied in Files; system clipboard unavailable";
        }
        labels();
    }
    void consume(std::size_t i, FilesPaneReply reply) {
        auto &p = panes[i];
        if (p.inspection && reply.request == p.inspection->ticket && reply.navigation == p.navigation) {
            const auto kind = p.inspection->kind;
            p.inspection.reset();
            if (kind == FilesPaneReplyKind::PathText)
                consume_path_text(i, std::move(reply));
            else if (kind == FilesPaneReplyKind::Link)
                consume_link(i, std::move(reply));
            else
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
            const bool was_create_file = p.creating_file;
            const bool was_create_hard_link = p.creating_hard_link;
            const bool was_create_symbolic_link = p.creating_symbolic_link;
            p.renaming = false;
            p.trashing = false;
            p.deleting = false;
            p.creating_file = false;
            p.creating_hard_link = false;
            p.creating_symbolic_link = false;
            p.model.set_input_locked(false);
            if (reply.state == FilesPaneReplyState::Ready && reply.mutation_applied.value_or(false)) {
                p.error.clear();
                // A confirmed mutation makes the old listing stale until a
                // worker publishes a verified refresh of the held directory.
                p.refresh_failed = true;
                auto saved_batch = std::move(p.batch_delete_names);
                p.batch_delete_names.clear();
                const bool refresh_started = refresh(i);
                p.batch_delete_names = std::move(saved_batch);
                if (!refresh_started) {
                    p.batch_delete_kind = Pane::BatchDeleteKind::None;
                    p.batch_delete_names.clear();
                    p.error = was_delete ? "Entry permanently deleted; refresh could not be started"
                                         : was_trash ? "Entry moved to Trash / Recycle Bin; refresh could not be started"
                                         : was_rename ? "Entry renamed; refresh could not be started"
                                                      : was_create_file ? "File created; refresh could not be started"
                                                                         : was_create_hard_link ? "Hard link created; refresh could not be started"
                                                                                                : was_create_symbolic_link ? "Symbolic link created; refresh could not be started"
                                                                                                : "Directory created; refresh could not be started";
                }
            } else {
                p.batch_delete_kind = Pane::BatchDeleteKind::None;
                p.batch_delete_names.clear();
                p.error = reply.state == FilesPaneReplyState::Ready
                              ? (was_delete ? "Permanent deletion outcome unavailable"
                                            : was_trash ? "Trash / Recycle Bin outcome unavailable"
                                            : was_rename ? "Rename outcome unavailable"
                                                         : was_create_file ? "File creation outcome unavailable"
                                                                            : was_create_hard_link ? "Hard-link creation outcome unavailable"
                                                                                                   : was_create_symbolic_link ? "Symbolic-link creation outcome unavailable"
                                                                                                   : "Directory creation outcome unavailable")
                              : failure(reply);
            }
            labels();
            return;
        }
        if (reply.navigation != p.navigation || reply.request != p.request)
            return;
        const bool navigation = reply.request == reply.navigation;
        const bool refreshing = p.refreshing;
        const bool reading_cursor = p.preparing_cursor;
        const bool preparing_view = p.changing == FilesPaneReplyKind::Focus || reading_cursor;
        auto queued_view = std::move(p.queued_view);
        p.queued_view.reset();
        const bool queued_refresh = std::exchange(p.queued_refresh, false);
        const auto queued_copy = std::exchange(p.queued_copy, std::nullopt);
        const bool cancelled = p.cancelling;
        p.stamp = reply.stamp;
        p.navigating = p.entering = p.homing = p.refreshing = false;
        p.reading = false;
        p.changing.reset();
        p.cancelling = false;
        p.preparing_cursor = false;
        p.model.set_input_locked(false);
        if (reply.kind == FilesPaneReplyKind::View && reply.change_outcome == FilesPaneChangeOutcome::Applied)
            p.range_start.reset();
        bool adopted = false;
        const bool replace_volumes = navigation && reply.state == FilesPaneReplyState::Ready && reply.active_location;
        if ((!p.volume_view || replace_volumes) && reply.page && reply.page->stamp().current()) {
            if (p.model.publish(reply.page, navigation || (reply.kind == FilesPaneReplyKind::View &&
                                                           reply.change_outcome == FilesPaneChangeOutcome::Applied))) {
                adopted = true;
                if (navigation) {
                    p.volume_view = false;
                    p.volume_inventory_stale = false;
                    p.active = reply.active_location;
                    p.requested = reply.requested_location;
                    p.read_only_mount = reply.read_only_mount;
                    p.navigation_error = false;
                    p.refresh_failed = false;
                    p.range_start.reset();
                    if (p.active && !refreshing) {
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
                p.error = refreshing ? "Listing refreshed before cancellation completed"
                                     : "Location opened before cancellation completed";
                p.change_error = true;
            } else if (cancelled && preparing_view) {
                p.error = queued_refresh ? "Refresh cancelled before scanning"
                        : queued_copy    ? (*queued_copy == ck::files::FileOperation::Move
                                                ? "Move cancelled before planning"
                                                : "Copy cancelled before planning")
                                         : "View change cancelled before application";
                p.change_error = true;
            } else if (cancelled && reply.change_outcome == FilesPaneChangeOutcome::Applied) {
                p.error = "Change applied before cancellation completed";
                p.change_error = true;
            }
            if ((queued_view || queued_refresh || queued_copy) && reading_cursor && !cancelled) {
                if (const auto target = p.model.target()) {
                    if (begin_change(i, FilesPaneFocusRow{target->row}, true)) {
                        p.queued_view = std::move(queued_view);
                        p.queued_refresh = queued_refresh;
                        p.queued_copy = queued_copy;
                    }
                } else {
                    p.error = queued_refresh ? "Refresh not started; current row was not delivered"
                            : queued_copy    ? "Operation not started; current row was not delivered"
                                             : "View not applied; current row was not delivered";
                    p.change_error = true;
                }
            } else if (queued_refresh && reply.kind == FilesPaneReplyKind::Focus && !cancelled &&
                       reply.change_outcome == FilesPaneChangeOutcome::Applied) {
                start_refresh(i);
            } else if (queued_view && reply.kind == FilesPaneReplyKind::Focus && !cancelled &&
                       reply.change_outcome == FilesPaneChangeOutcome::Applied) {
                begin_change(i, FilesPaneChangeView{std::move(*queued_view)}, true);
            } else if (queued_copy && i == active && reply.kind == FilesPaneReplyKind::Focus && !cancelled &&
                       reply.change_outcome == FilesPaneChangeOutcome::Applied) {
                prepare_copy(*queued_copy);
            } else if (reply.kind == FilesPaneReplyKind::Marks && !cancelled && !p.queued_marks.empty()) {
                drain_queued_marks(i);
            } else if (adopted && refreshing && !cancelled &&
                       p.batch_delete_kind != Pane::BatchDeleteKind::None && !p.batch_delete_names.empty()) {
                drain_batch_delete(i);
            }
            return;
        }
        p.error = reply.state == FilesPaneReplyState::Ready ? "Stale page rejected" : failure(reply);
        if (reply.change_outcome == FilesPaneChangeOutcome::Applied)
            p.error = (queued_refresh ? "Refresh not started; current row acknowledged; "
                       : queued_copy  ? "Operation not started; current row acknowledged; "
                       : preparing_view ? "View not applied; current row acknowledged; " : "Change applied; ") + p.error;
        else if (reply.change_outcome == FilesPaneChangeOutcome::Unknown)
            p.error = "Change outcome unknown; reopen location before further actions. " + p.error;
        else if (reply.change_outcome == FilesPaneChangeOutcome::NotApplied)
            p.error = "Change not applied; " + p.error;
        else if (preparing_view)
            p.error = (queued_refresh ? "Refresh not started; "
                       : queued_copy  ? "Operation not started; "
                                      : "View not applied; ") + p.error;
        p.change_error = preparing_view || reply.change_outcome != FilesPaneChangeOutcome::NotRequested;
        if (refreshing) {
            p.refresh_failed = true;
            p.error = "Refresh failed; " + p.error;
        }
        p.navigation_error = navigation && !refreshing;
        if (p.volume_view && replace_volumes)
            p.volume_inventory_stale = true;
        if (navigation && !p.volume_view && reply.active_location)
            p.active = reply.active_location;
        if (!adopted)
            p.blocked = p.inflight;
    }
    void page(std::size_t i) {
        auto &p = panes[i];
        if (p.volume_view || volume_pending(i) || p.navigating || p.changing ||
            p.queued_view || p.queued_refresh || p.queued_copy.has_value() || p.inspection || !p.active ||
            !surfaces[i]->visible() || !p.stamp.current() || !services[i]->running())
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
    bool archive_destination_current(const VolumeArchiveOrigin &origin) const {
        const auto &destination = panes[1 - origin.source_pane];
        return !destination.volume_view && !volume_pending(1 - origin.source_pane) &&
               !destination.navigating && destination.active &&
               destination.navigation == origin.destination_navigation &&
               *destination.active == *origin.destination;
    }
    bool archive_review_current(const VolumeArchiveOrigin &origin) const {
        const auto &source = panes[origin.source_pane];
        const auto volume = source.model.volume_cursor();
        return source.volume_view && source.navigation == origin.source_navigation && volume &&
               volume->identity == origin.volume.identity && volume->mount_point == origin.volume.mount_point &&
               archive_destination_current(origin);
    }
    void report_archive_error(std::string message) {
        if (volume_archive_origin)
            panes[volume_archive_origin->source_pane].error = std::move(message);
        volume_archive_origin.reset();
        labels();
    }
    bool report_read_only_archive_destination(const ck::files::UnavailableObservation &failure) {
        if (failure.state != ck::files::ObservationState::Denied ||
            failure.error.operation != "volume_archive.destination_read_only")
            return false;
        if (volume_archive_origin && archive_destination_current(*volume_archive_origin))
            panes[1 - volume_archive_origin->source_pane].read_only_mount = true;
        report_archive_error("Copy unavailable: destination mount is read-only");
        return true;
    }
    void consume_volume_archive_plan(FilesVolumeArchivePlanningReply reply) {
        if (!volume_archive_planning_request || reply.request != *volume_archive_planning_request)
            return;
        volume_archive_planning_request.reset();
        const auto ticket = reply.request;
        if (!volume_archive_origin) {
            volume_archive_service->cancel_plan(ticket);
            return;
        }
        if (reply.state != FilesVolumeArchivePlanningState::Ready || !reply.review) {
            if (reply.error && report_read_only_archive_destination(*reply.error))
                return;
            if (reply.error && reply.error->state == ck::files::ObservationState::Denied) {
                report_archive_error("Archive access denied. Check source and destination permissions.");
                return;
            }
            if (reply.error && (reply.error->error.operation == "volume_archive.source.entry_changed" ||
                                reply.error->error.operation == "volume_archive.source.directory_changed")) {
                report_archive_error("Archive source changed during capture. Start a new review.");
                return;
            }
            if (reply.error && (archive_storage_exhausted(reply.error->error.domain, reply.error->error.code) ||
                                reply.error->error.operation == "volume_archive.source.scratch_limit")) {
                report_archive_error("Archive capture storage or quota limit reached.");
                return;
            }
            if (reply.error && reply.error->error.operation == "volume_archive.source.image_limits") {
                report_archive_error("Archive exceeds the supported image size or extent count.");
                return;
            }
            if (reply.error && reply.error->error.operation == "volume_archive.manifest_limit") {
                report_archive_error("Archive exceeds the supported manifest or image size.");
                return;
            }
            if (reply.error && reply.error->error.operation == "volume_archive.source_evidence_limit") {
                report_archive_error("Volume identity or mount path is too long to record in the archive journal.");
                return;
            }
            if (reply.error && reply.error->error.operation == "volume_archive.source.version_unavailable") {
                report_archive_error("Archive source change information is unavailable.");
                return;
            }
            std::string message = reply.state == FilesVolumeArchivePlanningState::Cancelled
                ? "Volume archive cancelled" : "Volume archive planning failed";
            if (reply.error)
                message += ": " + reply.error->error.operation;
            report_archive_error(std::move(message));
            return;
        }
        if (!archive_review_current(*volume_archive_origin)) {
            volume_archive_service->cancel_plan(ticket);
            report_archive_error("Volume archive review became stale; start a new review");
            return;
        }
        const auto review = std::move(reply.review);
        volume_archive_review_dialog = std::make_unique<FilesVolumeArchiveReviewDialog>(
            application, shell->desktop(), shell->roles(), review,
            [this, weak = std::weak_ptr<void>(lifetime), ticket](
                ck::files::Observation<FilesVolumeArchiveApproval> result) {
                if (weak.expired())
                    return;
                volume_archive_review_dialog.reset();
                if (!result.value()) {
                    volume_archive_service->cancel_plan(ticket);
                    report_archive_error("Volume archive cancelled");
                    return;
                }
                if (!volume_archive_origin || !archive_review_current(*volume_archive_origin)) {
                    volume_archive_service->cancel_plan(ticket);
                    report_archive_error("Volume archive review became stale; start a new review");
                    return;
                }
                if (!volume_archive_service || !volume_archive_service->running()) {
                    report_archive_error("Volume archive worker is unavailable");
                    return;
                }
                const auto &origin = *volume_archive_origin;
                const auto submitted = volume_archive_service->execute_review(
                    ticket, std::move(result).take_value(), *operation_journal_root);
                if (submitted.state != FilesVolumeArchiveSubmission::Accepted) {
                    volume_archive_service->cancel_plan(ticket);
                    report_archive_error("Volume archive worker rejected the approved review");
                    return;
                }
                volume_archive_execution_request = submitted.ticket;
                panes[origin.source_pane].error.clear();
                watch();
                labels();
            });
        labels();
    }
    bool scan_archive_recovery() {
        volume_archive_recovery_scan.reset();
        if (!volume_archive_recovery_service || !operation_journal_root ||
            !operation_journal_root->is_valid()) {
            volume_archive_recovery_error = "archive journal is not configured";
            return false;
        }
        if (volume_archive_recovery_request)
            return true;
        if (volume_archive_recovery_service->request(*operation_journal_root) !=
            FilesVolumeArchiveRecoverySubmission::Accepted) {
            volume_archive_recovery_error = "archive recovery scan could not be started";
            return false;
        }
        volume_archive_recovery_error.clear();
        volume_archive_recovery_request = true;
        watch();
        return true;
    }
    void consume_volume_archive(FilesVolumeArchiveReply reply) {
        if (!volume_archive_execution_request || reply.request != *volume_archive_execution_request)
            return;
        volume_archive_execution_request.reset();
        // A failed or cancelled writer can leave a durable recovery record.
        // Refresh before Copy or recovery inspection can reuse the old scan.
        scan_archive_recovery();
        if (!volume_archive_origin)
            return;
        if (reply.state == FilesVolumeArchiveState::Failed && !reply.failure_pod && reply.error &&
            report_read_only_archive_destination(*reply.error))
            return;
        if (reply.state == FilesVolumeArchiveState::Complete ||
            reply.state == FilesVolumeArchiveState::Indeterminate) {
            const auto &origin = *volume_archive_origin;
            auto &destination = panes[1 - origin.source_pane];
            const auto &result = reply.result;
            const bool published = reply.state == FilesVolumeArchiveState::Complete ||
                                   (result && result->publication_confirmed);
            std::string message = reply.state == FilesVolumeArchiveState::Complete
                ? "Volume archive created"
                : published ? "Volume archive published; durability is indeterminate"
                            : "Volume archive publication is indeterminate";
            const bool verified_path = result && result->published_path;
            if (verified_path) {
                const auto address = ck::files::NativeAbsolutePath::from_host(*result->published_path);
                if (address.value())
                    message += ": " + FilesPaneLocation(*address.value()).display_text();
            } else if (published) {
                message = reply.state == FilesVolumeArchiveState::Complete
                    ? "Volume archive created in the reviewed folder. Its current path could not be verified."
                    : "Volume archive published in the reviewed folder. Its current path and durability could not be verified.";
            }
            if (reply.failure_pod)
                message += ": " + std::string(reply.failure_pod->operation.data());
            destination.archive_notice = std::move(message);
            if (verified_path && archive_destination_current(origin)) {
                destination.refresh_failed = true;
                destination.archive_refresh = origin.destination_navigation;
            }
        } else {
            std::string message = reply.state == FilesVolumeArchiveState::Cancelled
                ? "Volume archive cancelled" : "Volume archive failed";
            const bool storage_exhausted = reply.failure_pod
                ? archive_storage_exhausted(reply.failure_pod->domain, reply.failure_pod->code)
                : reply.error && archive_storage_exhausted(reply.error->error.domain, reply.error->error.code);
            if (storage_exhausted) {
                message = "Volume archive failed: storage or quota limit reached.";
            } else if (reply.failure_pod) {
                message += ": " + std::string(reply.failure_pod->operation.data());
            } else if (reply.error) {
                const auto &operation = reply.error->error.operation;
                if (operation == "volume_archive.source.mount_changed")
                    message = "Source volume mount changed. Start a new review.";
                else if (operation == "volume_archive.review.destination_changed")
                    message = "Archive destination changed. Start a new review.";
                else if (operation == "volume_archive.review.destination_conflict")
                    message = "An entry now uses the archive name. Start a new review.";
                else if (operation == "volume_archive.recovery_admission_required")
                    message = "An archive journal needs recovery review before copying.";
                else if (operation == "volume_archive.recovery_root_changed" ||
                         operation == "volume_archive.recovery_directory_changed")
                    message = "The archive journal directory changed during inspection.";
                else
                    message += ": " + operation;
            }
            report_archive_error(std::move(message));
            return;
        }
        volume_archive_origin.reset();
        labels();
    }
    void refresh_archive_destination(std::size_t i) {
        auto &pane = panes[i];
        if (!pane.archive_refresh)
            return;
        if (*pane.archive_refresh != pane.navigation || pane.volume_view) {
            pane.archive_refresh.reset();
            return;
        }
        if (pending(pane) || volume_pending(i) || modal_pending() || operation_execution_request)
            return;
        pane.archive_refresh.reset();
        if (!refresh(i))
            pane.archive_notice += "; refresh could not be started";
    }
    void consume_planning(FilesOperationPlanningReply reply) {
        if (!operation_planning_request || reply.request != *operation_planning_request)
            return;
        const auto encoding = *operation_planning_encoding;
        auto revalidate = std::move(operation_planning_revalidation);
        auto completion = std::move(operation_planning_completion);
        const auto transfer = std::exchange(operation_planning_transfer, std::nullopt);
        operation_planning_request.reset();
        operation_planning_cancelling = false;
        operation_planning_encoding.reset();
        operation_planning_revalidation = {};
        operation_planning_completion = {};
        if (transfer && reply.state == FilesOperationPlanningReplyState::Cancelled) {
            const auto &[source, planning] = *transfer;
            panes[source].error = planning.operation == ck::files::FileOperation::Move
                ? "Move planning cancelled" : "Copy planning cancelled";
            return;
        }
        if (transfer && reply.state == FilesOperationPlanningReplyState::LimitExceeded) {
            const auto &[source, planning] = *transfer;
            const char *operation = planning.operation == ck::files::FileOperation::Move ? "Move" : "Copy";
            const bool too_many_entries = reply.error && reply.error->error.operation == "frozen_inventory.entries";
            panes[source].error = std::string(operation) + (too_many_entries
                ? " exceeds the " + std::to_string(planning.evidence_limits.max_items) + "-entry review limit"
                : " exceeds a planning limit");
            return;
        }
        if (transfer && reply.state == FilesOperationPlanningReplyState::ReconciliationRequired) {
            const auto &[source, planning] = *transfer;
            const char *operation = planning.operation == ck::files::FileOperation::Move ? "Move" : "Copy";
            if (reply.reconciliation && reply.reconciliation->has_retained_source_repeat())
                panes[source].error = std::string(operation) +
                    " blocked: previous move retained source; reconciliation required";
            else if (reply.reconciliation && reply.reconciliation->has_untrusted_journal())
                panes[source].error = std::string(operation) +
                    " blocked: untrusted operation journal requires reconciliation";
            else
                panes[source].error = std::string(operation) +
                    " blocked: overlapping operation requires reconciliation";
            scan_operation_recovery();
            if (reply.reconciliation)
                show_reconciliation_overlap(planning.operation, *reply.reconciliation);
            return;
        }
        if (reply.state == FilesOperationPlanningReplyState::Ready && reply.review) {
            if (transfer && reply.evidence && reply.plan->operation() == ck::files::FileOperation::Copy &&
                reply.evidence->inventory->mount_policy() != ck::files::InventoryMountPolicy::DirectObjects &&
                std::any_of(reply.plan->conflicts().begin(), reply.plan->conflicts().end(), [](const auto& conflict) {
                    return conflict.item == 0 && conflict.kind == ck::files::DestinationConflictKind::ExistingDirectory;
                })) {
                const auto &[source, planning] = *transfer;
                FilesOperationExecutionRequest execution{
                    ck::files::FileOperation::Copy, planning.source_service, planning.destination_service,
                    planning.source_navigation, planning.destination_navigation,
                    planning.source_stamp, planning.destination_stamp, planning.source_limits, *operation_journal_root, {}, {}, {},
                    std::move(reply.evidence), {}, true};
                start_transfer(std::move(execution), source, 1 - source);
                return;
            }
            const auto fallback = completion;
            const auto inventory = reply.evidence ? reply.evidence->inventory : nullptr;
            std::optional<FilesTransferReviewScope> scope;
            if (transfer && reply.evidence && reply.evidence->destination_address && inventory &&
                !inventory->entries().empty()) {
                auto source = inventory->entries().front().source_path.parent_address();
                if (source.value())
                    scope.emplace(FilesTransferReviewScope{std::move(source).take_value(),
                        *reply.evidence->destination_address, reply.evidence->destination_names});
            }
            std::function<bool(ck::files::ConsentAction)> supports_action;
            if (transfer && reply.evidence)
                supports_action = [evidence = reply.evidence](auto action) {
                    return files_transfer_action_supported(*evidence, action);
                };
            auto deliver = [deliver_result = std::move(completion), evidence = std::move(reply.evidence)](
                               ck::files::Observation<ck::files::ConsentReceipt> result) mutable {
                deliver_result(std::move(result), std::move(evidence));
            };
            if ((transfer && !scope) ||
                !review_operation(std::move(reply.review), encoding, std::move(revalidate), std::move(deliver),
                                  inventory, std::move(supports_action), std::move(scope)))
                fallback(ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                    ck::files::ObservationState::Failed,
                    {ck::files::ErrorDomain::Adapter,
                        static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                        "files_app.operation_review_start"}), {});
            return;
        }
        const auto detail = reply.error.value_or(ck::files::UnavailableObservation{
            ck::files::ObservationState::Failed,
            {ck::files::ErrorDomain::Adapter,
                static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                reply.state == FilesOperationPlanningReplyState::Cancelled
                    ? "files_app.operation_planning_cancelled" : "files_app.operation_planning"}});
        completion(ck::files::Observation<ck::files::ConsentReceipt>::unavailable(detail.state, detail.error), {});
    }
    bool scan_operation_recovery() {
        if (!operation_recovery_service)
            return false;
        operation_recovery_scan.reset();
        if (operation_recovery_request) {
            operation_recovery_dirty = true;
            return true;
        }
        operation_recovery_dirty = false;
        if (!operation_journal_root || !operation_journal_root->is_valid()) {
            operation_recovery_error = "operation journal is not configured";
            return false;
        }
        if (operation_recovery_service->request(*operation_journal_root) != FilesOperationRecoverySubmission::Accepted) {
            operation_recovery_error = "operation recovery scan could not be started";
            return false;
        }
        operation_recovery_error.clear();
        operation_recovery_request = true;
        return true;
    }
    void consume_execution(FilesOperationExecutionReply reply) {
        if (!operation_execution_request || reply.request != *operation_execution_request)
            return;
        operation_execution_request.reset();
        scan_operation_recovery();
        const auto origin = *std::exchange(operation_execution_origin, std::nullopt);
        const auto operation = origin.kind;
        const auto source = origin.source_pane;
        const auto destination = 1 - source;
        const auto still_displays = [&](std::size_t pane, const FilesPaneTicket &navigation) {
            return panes[pane].navigation == navigation && !panes[pane].volume_view;
        };
        const bool source_current = still_displays(source, origin.source_navigation);
        const bool destination_current = operation != ck::files::FileOperation::Delete &&
            still_displays(destination, origin.destination_navigation);
        const auto skipped = reply.skipped_items.empty() ? std::string{} :
            std::to_string(reply.skipped_items.size()) +
                (reply.skipped_items.size() == 1 ? " conflict skipped" : " conflicts skipped");
        const auto retained = reply.retained_targets == 0 ? std::string{} :
            std::to_string(reply.retained_targets) +
                (reply.retained_targets == 1 ? " previous target in Operation recovery" : " previous targets in Operation recovery");
        const auto operation_notice = skipped.empty() ? retained :
            retained.empty() ? skipped : skipped + ". " + retained;
        if (reply.state == FilesOperationExecutionState::Complete && reply.result.items.empty() && !skipped.empty()) {
            if (source_current)
                panes[source].error = skipped;
            labels();
            return;
        }
        const auto refresh_affected = [&](std::size_t pane) {
            panes[pane].refresh_failed = true;
            return refresh(pane);
        };
        if (reply.state == FilesOperationExecutionState::Complete ||
            reply.state == FilesOperationExecutionState::RetainedSource) {
            const auto retention_message = [&] {
                std::string detail = operation == ck::files::FileOperation::Move ? "Move" : "Copy";
                detail += ": destination published, source retained";
                if (reply.error) {
                    if (reply.error->error.operation == "native_operation_copy_tree.source_stability")
                        detail += "; source stability could not be verified";
                    else
                        detail += ": " + reply.error->error.operation;
                }
                return detail;
            };
            const bool source_refreshed = !source_current || refresh_affected(source);
            const bool destination_refreshed = !destination_current || refresh_affected(destination);
            if (!source_refreshed || !destination_refreshed) {
                const auto label = operation == ck::files::FileOperation::Move ? "Move" :
                    operation == ck::files::FileOperation::Delete ? "Delete" : "Copy";
                auto detail = reply.state == FilesOperationExecutionState::RetainedSource
                    ? retention_message() : std::string(label) + " completed";
                detail += "; refresh could not be started";
                if (!operation_notice.empty())
                    detail += ". " + operation_notice;
                panes[source].error = std::move(detail);
                if (destination_current)
                    panes[destination].error = panes[source].error;
            } else if (reply.state == FilesOperationExecutionState::RetainedSource || !operation_notice.empty()) {
                const auto detail = reply.state == FilesOperationExecutionState::RetainedSource
                    ? retention_message() + (operation_notice.empty() ? "" : ". " + operation_notice) : operation_notice;
                panes[source].error = detail;
                panes[source].change_error = true; // Keep the outcome through the queued refresh.
                if (destination_current) {
                    panes[destination].error = detail;
                    panes[destination].change_error = true;
                }
            }
        } else {
            const std::string label = operation == ck::files::FileOperation::Move ? "Move" :
                operation == ck::files::FileOperation::Delete ? "Delete" : "Copy";
            std::string detail;
            if (reply.state == FilesOperationExecutionState::Cancelled)
                detail = label + " cancelled";
            else if (reply.state == FilesOperationExecutionState::NeedsReconciliation)
                detail = label + " needs reconciliation";
            else
                detail = label + " failed";
            if (reply.error) {
                if (reply.error->state == ck::files::ObservationState::Unsupported &&
                    (reply.error->error.operation == "create_staging_file.fd_publication_unqualified" ||
                     reply.error->error.operation == "preflight.fd_publication_unqualified"))
                    detail += ": verified file publication is unavailable on this platform";
                else if (reply.error->state == ck::files::ObservationState::Unsupported &&
                         (reply.error->error.operation == "staging.darwin_nonlocal_mount" ||
                          reply.error->error.operation == "staging.darwin_rename_excl_unsupported"))
                    detail += ": verified file publication requires a local volume supporting exclusive atomic rename";
                else if (reply.error->state == ck::files::ObservationState::Unsupported &&
                         reply.error->error.operation == "preflight.source_exclusion_unqualified")
                    detail += ": verified source retirement is unavailable on this platform";
                else if (reply.error->error.operation == "files_execution.keep_both_occupied")
                    detail += ": the new name already exists. Review the operation again";
                else if (reply.error->error.operation == "files_execution.keep_both_duplicate")
                    detail += ": the new name conflicts with another transfer name";
                else if (reply.error->error.operation == "files_execution.keep_both_name")
                    detail += ": the destination cannot use the new name";
                else if (reply.error->error.operation == "files_execution.reconciliation_overlap")
                    detail += ": an overlapping operation journal requires reconciliation";
                else if (reply.error->error.domain == ck::files::ErrorDomain::Adapter &&
                         reply.error->error.code == static_cast<std::int64_t>(ck::files::AdapterError::PlanDecisionStale))
                    detail += ": source or destination changed since review. Review the operation again";
                else
                    detail += ": " + reply.error->error.operation;
            }
            else if (reply.worker_fault != FilesPaneWorkerFault::None)
                detail += ": worker stopped";
            if (!operation_notice.empty())
                detail += ". " + operation_notice;
            panes[source].error = std::move(detail);
            if (reply.state == FilesOperationExecutionState::NeedsReconciliation && reply.reconciliation)
                show_reconciliation_overlap(operation, *reply.reconciliation);
        }
        labels();
    }

    void show_tree_copy_review(FilesTreeCopyReview review) {
        using namespace ck::files;
        using namespace ckv::widgets;
        if (!operation_execution_request || *operation_execution_request != review.request)
            return;
        if (modal_pending()) {
            execution_service->cancel(review.request);
            return;
        }
        DialogDescriptor descriptor;
        descriptor.title = "Review folder copy";
        descriptor.resizable = true;
        add_files_transfer_scope(descriptor, {std::move(review.source), std::move(review.destination),
                                 {std::move(review.selected_name)}}, application, shell->desktop(), shell->roles());
        descriptor.fields.push_back({.label = "Merge the two folders and keep files that exist only at the destination.",
                                     .kind = FieldKind::Note});
        const auto conflicts = review.conflicts;
        struct RowField final {
            std::uint64_t row;
            std::size_t field;
        };
        std::vector<RowField> row_fields;
        for (const auto& row : review.rows) {
            const auto& item = row.item;
            std::string name;
            for (const auto& component : item.relative_components) {
                if (!name.empty())
                    name += '/';
                name += component.display_name();
            }
            const auto action = item.row.requires_review ? "Conflict" :
                item.action == ReviewedTreeAction::MergeDirectory ? "Merge folder" :
                item.action == ReviewedTreeAction::CreateDirectory ? "Create folder" :
                item.action == ReviewedTreeAction::Copy ? "Copy" : "Keep";
            auto label = std::string(action) + "  " + files_review_literal(name);
            if (item.row.requires_review && item.facts.source_size && item.facts.destination_size)
                label += "  (" + std::to_string(*item.facts.source_size) + " → " +
                    std::to_string(*item.facts.destination_size) + " bytes)";
            if (review.can_apply && item.row.requires_review) {
                std::vector<std::string> options{"Use folder decision", "Keep existing"};
                if (row.can_replace) {
                    options.push_back("Use source and retain target");
                    options.push_back("Use newest and retain target");
                }
                row_fields.push_back({item.row.id, descriptor.fields.size()});
                descriptor.fields.push_back({.label = std::move(label), .kind = FieldKind::Combo,
                                             .options = std::move(options), .initial_selection = 0});
            } else {
                descriptor.fields.push_back({.label = std::move(label), .kind = FieldKind::Note});
            }
        }
        std::vector<std::string> choices{"Cancel"};
        if (review.can_apply) {
            choices.push_back(conflicts ? "Keep existing on conflicts" : "Copy missing items and merge folders");
            if (conflicts && review.can_use_source) {
                choices.push_back("Use source and retain the previous target");
                choices.push_back("Use newest and retain replaced targets");
            } else if (conflicts) {
                descriptor.fields.push_back({.label = "Replacement choices appear on rows with verified retention.",
                                             .kind = FieldKind::Note});
            }
        } else {
            descriptor.fields.push_back({.label = "Folder copy unavailable: the worker could not verify required preservation.",
                                         .kind = FieldKind::Note});
        }
        if (review.can_apply && std::any_of(review.rows.begin(), review.rows.end(), [](const auto& row) { return row.can_replace; }))
            descriptor.fields.push_back({.label = "Use newest keeps the existing file when modification times tie.",
                                         .kind = FieldKind::Note});
        const auto choice_field = descriptor.fields.size();
        descriptor.fields.push_back({.label = "&Conflict resolution", .kind = FieldKind::Combo,
                                     .options = std::move(choices), .initial_selection = 0});
        if (review.can_apply)
            descriptor.buttons.push_back({"&Apply folder copy", ButtonRole::Accept, {}});
        descriptor.buttons.push_back({"&Cancel", ButtonRole::Dismiss, {}});
        tree_copy_dialog.emplace(present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        tree_copy_window = shell->desktop().active_window();
        if (tree_copy_window)
            tree_copy_window_lifetime = tree_copy_window->lifetime_token();
        tree_copy_dialog->set_completion_handler([this, weak = std::weak_ptr<void>(lifetime), ticket = review.request,
                                                  plan = review.plan, choice_field, fields = std::move(row_fields)](
                                                     const DialogResult& answer) {
            if (weak.expired())
                return;
            tree_copy_window = nullptr;
            tree_copy_dialog.reset();
            const auto choice_at = [&](std::size_t field) {
                const auto selected = answer.accepted && field < answer.selected.size() ? answer.selected[field] : 0;
                switch (selected) {
                case 1: return ConflictChoice::KeepTarget;
                case 2: return ConflictChoice::UseSource;
                case 3: return ConflictChoice::Newest;
                default: return ConflictChoice::Cancel;
                }
            };
            const auto choice = choice_at(choice_field);
            std::vector<FilesTreeConflictDecision> overrides;
            if (choice != ConflictChoice::Cancel)
                for (const auto& row : fields) {
                    const auto selected = choice_at(row.field);
                    if (selected != ConflictChoice::Cancel)
                        overrides.push_back({row.row, selected});
                }
            if (choice == ConflictChoice::Cancel ||
                !execution_service->apply_tree_review(ticket, plan, choice, std::move(overrides)))
                execution_service->cancel(ticket);
            labels();
        });
        labels();
    }
    void pump() {
        // Leave replies in their worker mailboxes while the user decides.
        // Consuming a review under the quit modal could reject or cancel it.
        if (closing || pumping || confirmation)
            return;
        pumping = true;
        try {
            poll_settings();
            if (planning_service) {
                if (auto reply = planning_service->take())
                    consume_planning(std::move(*reply));
                if (operation_planning_request && !planning_service->running()) {
                    auto completion = std::move(operation_planning_completion);
                    operation_planning_request.reset();
                    operation_planning_cancelling = false;
                    operation_planning_encoding.reset();
                    operation_planning_revalidation = {};
                    operation_planning_completion = {};
                    operation_planning_transfer.reset();
                    completion(ck::files::Observation<ck::files::ConsentReceipt>::unavailable(
                        ck::files::ObservationState::Failed,
                        {ck::files::ErrorDomain::Adapter,
                            static_cast<std::int64_t>(ck::files::AdapterError::InterruptedRead),
                            "files_app.operation_planning_worker"}), {});
                }
            }
            if (volume_archive_service) {
                if (auto reply = volume_archive_service->take_plan())
                    consume_volume_archive_plan(std::move(*reply));
                if (volume_archive_planning_request && !volume_archive_service->running()) {
                    volume_archive_planning_request.reset();
                    report_archive_error("Volume archive planning worker stopped");
                }
            }
            if (volume_archive_recovery_service) {
                if (auto reply = volume_archive_recovery_service->take()) {
                    volume_archive_recovery_request = false;
                    if (reply->state == FilesVolumeArchiveRecoveryState::Ready && reply->scan) {
                        volume_archive_recovery_scan = std::make_shared<const FilesVolumeArchiveRecoveryScan>(
                            std::move(*reply->scan));
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
            if (volume_archive_recovery_cleanup_service) {
                if (auto reply = volume_archive_recovery_cleanup_service->take()) {
                    volume_archive_recovery_cleanup_request = false;
                    if (reply->state == FilesVolumeArchiveRecoveryCleanupState::Complete && reply->removed) {
                        volume_archive_recovery_cleanup_error.clear();
                        if (!scan_archive_recovery())
                            volume_archive_recovery_cleanup_error =
                                "Archive staging cleanup completed; recovery rescan could not be started";
                    } else {
                        volume_archive_recovery_cleanup_error = reply->error
                            ? "Archive staging cleanup failed: " + reply->error->error.operation
                            : "Archive staging cleanup failed";
                    }
                    labels();
                }
                if (volume_archive_recovery_cleanup_request &&
                    !volume_archive_recovery_cleanup_service->running()) {
                    volume_archive_recovery_cleanup_request = false;
                    volume_archive_recovery_cleanup_error = "Archive staging cleanup worker stopped";
                    labels();
                }
            }
            if (operation_recovery_service) {
                if (auto reply = operation_recovery_service->take()) {
                    operation_recovery_request = false;
                    if (operation_recovery_dirty) {
                        scan_operation_recovery();
                    } else if (reply->state == FilesOperationRecoveryState::Ready && reply->scan) {
                        operation_recovery_scan = std::move(reply->scan);
                        operation_recovery_error.clear();
                    } else {
                        operation_recovery_scan.reset();
                        operation_recovery_error = reply->error
                            ? "operation recovery scan failed: " + reply->error->error.operation
                            : "operation recovery scan failed";
                    }
                    labels();
                }
                if (auto action_reply = operation_recovery_service->take_action()) {
                    operation_recovery_action_pending = false;
                    if (action_reply->state == FilesOperationRecoveryState::Ready && action_reply->result) {
                        operation_recovery_action_status = action_reply->result->summary;
                        operation_recovery_error.clear();
                        for (std::size_t pane_idx = 0; pane_idx < 2; ++pane_idx)
                            if (ready(pane_idx) && !panes[pane_idx].volume_view)
                                refresh(pane_idx);
                        scan_operation_recovery();
                    } else {
                        operation_recovery_action_status = action_reply->error
                            ? "Recovery action failed: " + action_reply->error->error.operation
                            : "Recovery action failed";
                    }
                    labels();
                }
                if ((operation_recovery_request || operation_recovery_action_pending) &&
                    !operation_recovery_service->running()) {
                    operation_recovery_request = false;
                    operation_recovery_action_pending = false;
                    operation_recovery_dirty = false;
                    operation_recovery_error = "operation recovery worker stopped";
                    labels();
                }
            }
            if (operation_recovery_cleanup_service) {
                if (auto reply = operation_recovery_cleanup_service->take()) {
                    operation_recovery_cleanup_request = false;
                    if (reply->state == FilesOperationRecoveryCleanupState::Complete) {
                        operation_recovery_cleanup_error.clear();
                        if (!scan_operation_recovery())
                            operation_recovery_cleanup_error =
                                "Operation staging cleanup completed; recovery rescan could not be started";
                    } else {
                        operation_recovery_cleanup_error = reply->error
                            ? "Operation staging cleanup failed: " + reply->error->error.operation
                            : "Operation staging cleanup failed";
                    }
                    labels();
                }
                if (operation_recovery_cleanup_request &&
                    !operation_recovery_cleanup_service->running()) {
                    operation_recovery_cleanup_request = false;
                    operation_recovery_cleanup_error = "Operation staging cleanup worker stopped";
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
                    panes[i].inspection.reset();
                    panes[i].mutating = panes[i].renaming = panes[i].trashing = false;
                    panes[i].changing.reset();
                    panes[i].queued_view.reset();
                    panes[i].queued_refresh = false;
                    panes[i].queued_copy.reset();
                    panes[i].model.set_input_locked(false);
                    panes[i].error = changing ? "Worker stopped; change outcome unknown" : "Worker stopped";
                }
                page(i);
            }
            if (execution_service) {
                if (auto reply = execution_service->take())
                    consume_execution(std::move(*reply));
                if (auto review = execution_service->take_tree_review())
                    show_tree_copy_review(std::move(*review));
                if (operation_execution_request && !execution_service->running()) {
                    operation_execution_request.reset();
                    scan_operation_recovery();
                    const auto origin = *std::exchange(operation_execution_origin, std::nullopt);
                    panes[origin.source_pane].error = origin.kind == ck::files::FileOperation::Move
                        ? "Move worker stopped"
                        : origin.kind == ck::files::FileOperation::Delete
                            ? "Delete worker stopped" : "Copy worker stopped";
                    labels();
                }
            }
            if (volume_archive_service) {
                if (auto reply = volume_archive_service->take())
                    consume_volume_archive(std::move(*reply));
                if (volume_archive_execution_request && !volume_archive_service->running()) {
                    volume_archive_execution_request.reset();
                    report_archive_error("Volume archive worker stopped");
                }
            }
            if (volume_service) {
                if (auto reply = volume_service->take())
                    consume_volume(std::move(*reply));
                if (volume_request && !volume_service->running()) {
                    const auto pane = volume_request->pane;
                    if (volume_pending(pane))
                        panes[pane].error = "Volume information worker stopped";
                    volume_request.reset();
                }
            }
            for (std::size_t i = 0; i < panes.size(); ++i)
                refresh_archive_destination(i);
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
        if (!closing && operation_planning_request && planning_service) {
            if (!operation_planning_cancelling && planning_service->cancel(*operation_planning_request))
                operation_planning_cancelling = true;
            labels();
            return;
        }
        if (!closing && volume_archive_planning_request && volume_archive_service) {
            if (!volume_archive_planning_cancelling && volume_archive_service->cancel_plan(*volume_archive_planning_request))
                volume_archive_planning_cancelling = true;
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
        if (!closing && !modal_pending() && volume_pending(active)) {
            // Native enumeration may still be waiting for storage. Keep its
            // worker slot occupied until it returns and discard that reply.
            volume_request->retired = true;
            panes[active].error = "Volume scan cancelled";
            labels();
            return;
        }
        if (!closing && !modal_pending() && panes[active].volume_view && !panes[active].navigating) {
            (void)navigate_relative(active, false);
            return;
        }
        if (!closing && !modal_pending() && pending(panes[active])) {
            const auto &pane = panes[active];
            const auto request = pane.inspection ? pane.inspection->ticket : pane.request;
            services[active]->cancel(request);
            panes[active].cancelling = true;
            labels();
        }
    }
    void finish_exit() {
        if (closing)
            return;
        if (settings.worker)
            settings.worker->request_close(); // Seal + drain latest accepted snapshot; never join in dispatch.
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
        if (volume_archive_service)
            volume_archive_service->request_stop();
        if (volume_archive_recovery_service)
            volume_archive_recovery_service->request_stop();
        if (volume_archive_recovery_cleanup_service)
            volume_archive_recovery_cleanup_service->request_stop();
        if (operation_recovery_service)
            operation_recovery_service->request_stop();
        if (operation_recovery_cleanup_service)
            operation_recovery_cleanup_service->request_stop();
        host_quit(); // No join in dispatch; the composition owner joins afterwards.
    }
    void request_close() {
        if (closing || confirmation)
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
        if (bookmark_dialog && !bookmark_dialog->completed()) {
            close_after_bookmark = true;
            if (bookmark_dialog_window && !bookmark_dialog_lifetime.expired())
                bookmark_dialog_window->close();
            return;
        }
        if (properties_open()) {
            properties_dialog_window->close();
            return;
        }
        if (preview_open()) {
            preview_dialog_window->close();
            return;
        }
        if (link_dialog && !link_dialog->completed()) {
            if (link_dialog_window && !link_dialog_lifetime.expired())
                link_dialog_window->close();
            return;
        }
        if (create_directory_dialog && !create_directory_dialog->completed()) {
            if (create_directory_window && !create_directory_lifetime.expired())
                create_directory_window->close();
            return;
        }
        if (create_file_dialog && !create_file_dialog->completed()) {
            if (create_file_window && !create_file_lifetime.expired())
                create_file_window->close();
            return;
        }
        if (create_hard_link_dialog && !create_hard_link_dialog->completed()) {
            if (create_hard_link_window && !create_hard_link_lifetime.expired())
                create_hard_link_window->close();
            return;
        }
        if (create_symbolic_link_dialog && !create_symbolic_link_dialog->completed()) {
            if (create_symbolic_link_window && !create_symbolic_link_lifetime.expired())
                create_symbolic_link_window->close();
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
        if (archive_recovery_open()) {
            if (auto *active_window = shell->desktop().active_window())
                active_window->close();
            return;
        }
        if (archive_cleanup_dialog && !archive_cleanup_dialog->completed()) {
            if (archive_cleanup_window && !archive_cleanup_lifetime.expired())
                archive_cleanup_window->close();
            return;
        }
        if (operation_recovery_dialog && !operation_recovery_dialog->completed()) {
            if (operation_recovery_window && !operation_recovery_lifetime.expired())
                operation_recovery_window->close();
            return;
        }
        if (operation_review_dialog && !operation_review_dialog->completed()) {
            operation_review_dialog->close();
            return;
        }
        if (volume_archive_review_dialog && !volume_archive_review_dialog->completed()) {
            volume_archive_review_dialog->close();
            return;
        }
        if (tree_copy_dialog && !tree_copy_dialog->completed()) {
            if (tree_copy_window && !tree_copy_window_lifetime.expired())
                tree_copy_window->close();
            return;
        }
        if (parity_dialog && !parity_dialog->completed()) {
            if (auto *active_window = shell->desktop().active_window())
                active_window->close();
            return;
        }
        if (parity_report_window && !parity_report_lifetime.expired()) {
            parity_report_window->close();
            return;
        }
        const bool dirty_editors = has_dirty_embedded_editors();
        const bool operation_active = operation_execution_request || volume_archive_execution_request ||
            volume_archive_recovery_cleanup_request || operation_recovery_cleanup_request ||
            panes[0].mutating || panes[1].mutating;
        const bool planning_active = operation_planning_request || volume_archive_planning_request;
        const bool completion_active = std::any_of(completion_services.begin(), completion_services.end(),
            [](const FilesCompletionService *service) { return service && service->working(); });
        if (!dirty_editors && !operation_active && !planning_active && !completion_active && !services[0]->working() &&
            !services[1]->working() && !volume_request && !volume_archive_recovery_request &&
            !operation_recovery_request) {
            finish_exit();
            return;
        }
        exit_selected = false;
        ckv::widgets::DialogDescriptor descriptor;
        descriptor.title = "Close Files";
        descriptor.fields.push_back({.label = dirty_editors
                ? "Embedded editor has unsaved changes."
                : (operation_active ? "A file operation is pending." : "Files is still reading or checking."),
            .kind = ckv::widgets::FieldKind::Note});
        if (dirty_editors)
            descriptor.fields.push_back({.label = "Unsaved editor changes will be lost if you exit.",
                .kind = ckv::widgets::FieldKind::Note});
        if (operation_active)
            descriptor.fields.push_back({.label = "Completed changes remain.", .kind = ckv::widgets::FieldKind::Note});
        if (!dirty_editors)
            descriptor.fields.push_back({.label = "Stopping may wait for storage.", .kind = ckv::widgets::FieldKind::Note});
        descriptor.buttons = {
            {"&Stay", ckv::widgets::ButtonRole::Accept, {}},
            {"Stop activity and e&xit", ckv::widgets::ButtonRole::Dismiss, [this] { exit_selected = true; }}};
        confirmation.emplace(
            ckv::widgets::present_modal_dialog(std::move(descriptor), application, shell->desktop()));
        confirmation->set_completion_handler(
            [this, weak = std::weak_ptr<void>(lifetime)](const ckv::widgets::DialogResult &) {
                if (weak.expired())
                    return;
                confirmation.reset();
                if (exit_selected)
                    finish_exit();
                else
                    wake.request();
            });
    }

    bool review_operation(ck::files::OperationReviewModel::Pointer model,
        ck::files::NameEncoding destination_encoding, FilesApp::OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion, ck::files::FrozenInventory::Pointer inventory = {},
        std::function<bool(ck::files::ConsentAction)> supports_action = {},
        std::optional<FilesTransferReviewScope> scope = {}) {
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
            }, std::move(inventory), std::move(supports_action), std::move(scope));
        return true;
    }
    template<class Request>
    bool submit_operation_planning(Request request, ck::files::NameEncoding destination_encoding,
        FilesApp::OperationRevalidation revalidate, PlanningCompletion completion, bool transfer = false) {
        if (closing || modal_pending() || !planning_service || !revalidate || !completion ||
            !planning_service->running())
            return false;
        std::optional<std::pair<std::size_t, FilesOperationPanePlanningRequest>> transfer_request;
        if constexpr (std::is_same_v<Request, FilesOperationPanePlanningRequest>)
            if (transfer)
                transfer_request.emplace(active, request);
        const auto submitted = planning_service->request(std::move(request));
        if (submitted.state != FilesOperationPlanningSubmission::Accepted)
            return false;
        operation_planning_request = submitted.ticket;
        operation_planning_encoding = destination_encoding;
        operation_planning_revalidation = std::move(revalidate);
        operation_planning_completion = std::move(completion);
        operation_planning_transfer = std::move(transfer_request);
        watch();
        labels();
        return true;
    }
    template<class Request>
    bool prepare_operation(Request request,
        ck::files::NameEncoding destination_encoding, FilesApp::OperationRevalidation revalidate,
        FilesOperationReviewDialog::Completion completion) {
        if (!completion)
            return false;
        return submit_operation_planning(std::move(request), destination_encoding, std::move(revalidate),
            [deliver = std::move(completion)](auto result, auto) { deliver(std::move(result)); });
    }
};
FilesApp::FilesApp(ckv::ui::Application &application, std::array<FilesPaneService *, 2> services, FilesAppWake &wake,
                   std::array<ck::files::NativeAbsolutePath, 2> locations, std::function<void()> host_quit,
                   ThemeScheme theme, std::array<FilesCompletionService *, 2> completion_services,
                   FilesOperationPlanningService *planning_service,
                   FilesOperationExecutionService *execution_service,
                   std::shared_ptr<const ck::files::NativeAbsolutePath> operation_journal_root,
                   FilesSettingsBinding settings, FilesVolumeService *volume_service,
                   FilesVolumeArchiveService *volume_archive_service,
                   FilesVolumeArchiveRecoveryService *volume_archive_recovery_service,
                   FilesVolumeArchiveRecoveryCleanupService *volume_archive_recovery_cleanup_service,
                   FilesOperationRecoveryService *operation_recovery_service,
                   FilesOperationRecoveryCleanupService *operation_recovery_cleanup_service,
                   std::shared_ptr<const ck::files::NativeAbsolutePath> volume_archive_scratch_root)
    : state_(std::make_unique<State>(application, services, wake, std::move(locations), std::move(host_quit), theme,
                                     completion_services, planning_service, execution_service,
                                     std::move(operation_journal_root), std::move(settings), volume_service,
                                     volume_archive_service,
                                     volume_archive_recovery_service, volume_archive_recovery_cleanup_service,
                                     operation_recovery_service, operation_recovery_cleanup_service,
                                     std::move(volume_archive_scratch_root))) {}
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
bool FilesApp::create_file(std::size_t pane, ck::files::NativeName name) {
    return state_->create_file(pane, std::move(name));
}
bool FilesApp::rename(std::size_t pane, ck::files::NativeName name) {
    const auto target = state_->panes.at(pane).model.target();
    if (!target)
        return false;
    return state_->rename(pane, *target, std::move(name));
}
bool FilesApp::trash(std::size_t pane) {
    if (state_->panes.at(pane).selection.marks && state_->panes.at(pane).selection.marks->total > 0)
        return state_->start_batch_delete(pane, State::Pane::BatchDeleteKind::Trash);
    const auto target = state_->panes.at(pane).model.target();
    if (!target)
        return false;
    return state_->trash(pane, *target);
}
bool FilesApp::permanently_delete(std::size_t pane) {
    if (state_->panes.at(pane).selection.marks && state_->panes.at(pane).selection.marks->total > 0)
        return state_->start_batch_delete(pane, State::Pane::BatchDeleteKind::PermanentDelete);
    const auto target = state_->panes.at(pane).model.target();
    if (!target)
        return false;
    return state_->permanently_delete(pane, *target);
}
bool FilesApp::swap_panes() { return state_->swap_panes(); }
bool FilesApp::sync_other_pane() { return state_->sync_other_pane(); }
bool FilesApp::mark_pattern(std::size_t pane, std::string pattern, bool marked) {
    return state_->mark_pattern(pane, std::move(pattern), marked);
}
bool FilesApp::compare_directories() { return state_->compare_directories(); }
bool FilesApp::execute_recovery_action(std::size_t journal_index, FilesOperationRecoveryActionKind kind) {
    return state_->execute_recovery_action(journal_index, kind);
}
bool FilesApp::open_search() { return state_->show_search(); }
bool FilesApp::open_directory_tree() { return state_->show_directory_tree(); }
bool FilesApp::open_sync_directories() { return state_->show_sync_directories(); }
bool FilesApp::open_diff_files() { return state_->show_diff_files(); }
bool FilesApp::open_archive_browse() { return state_->show_archive_browse(); }
bool FilesApp::open_archive_create() { return state_->show_archive_create(); }
bool FilesApp::open_archive_extract() { return state_->show_archive_extract(); }
bool FilesApp::open_embedded_editor(bool new_document) { return state_->show_embedded_editor(new_document); }
bool FilesApp::open_embedded_terminal() { return state_->show_embedded_terminal(); }
bool FilesApp::open_run_command() { return state_->show_run_command(); }
bool FilesApp::open_user_menu() { return state_->show_user_menu(); }
bool FilesApp::open_connect_remote() { return state_->show_connect_remote(); }
bool FilesApp::open_inspect_volume_archive() { return state_->show_inspect_volume_archive(); }
bool FilesApp::open_restore_volume_archive() { return state_->show_restore_volume_archive(); }
bool FilesApp::parity_dialog_pending() const {
    return (state_->parity_dialog && !state_->parity_dialog->completed()) ||
           (state_->parity_report_window && !state_->parity_report_lifetime.expired());
}
std::size_t FilesApp::embedded_editor_count() const {
    std::size_t count = 0;
    for (const auto &ed : state_->embedded_editors) {
        if (ed.window && !ed.lifetime.expired())
            ++count;
    }
    return count;
}
bool FilesApp::has_dirty_embedded_editors() const {
    return state_->has_dirty_embedded_editors();
}
bool FilesApp::save_embedded_editor(std::size_t index, bool force_overwrite) {
    return state_->save_embedded_editor(index, force_overwrite);
}
bool FilesApp::edit_embedded_editor_text_for_test(std::size_t index, std::string text) {
    state_->prune_embedded_editors();
    if (index >= state_->embedded_editors.size() || !state_->embedded_editors[index].document)
        return false;
    state_->embedded_editors[index].document->set_text(std::move(text));
    return true;
}
bool FilesApp::embedded_terminal_open() const {
    return state_->embedded_terminal_window && !state_->embedded_terminal_lifetime.expired();
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
    return state_->properties_open();
}
bool FilesApp::preview_pending() const {
    return std::any_of(state_->panes.begin(), state_->panes.end(), [](const auto &pane) {
               return pane.inspection && pane.inspection->kind != FilesPaneReplyKind::PathText;
           }) ||
           state_->preview_open();
}
bool FilesApp::operation_review_pending() const {
    return (state_->operation_review_dialog && !state_->operation_review_dialog->completed()) ||
        (state_->tree_copy_dialog && !state_->tree_copy_dialog->completed());
}
bool FilesApp::location_editor_pending() const {
    return state_->location_dialog && !state_->location_dialog->completed();
}
bool FilesApp::volume_info_pending() const {
    return state_->volume_request.has_value();
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
    return State::pending(p) || state_->volume_pending(pane);
}
std::string FilesApp::status(std::size_t pane) const { return state_->status(pane); }
FilesCommandAvailability FilesApp::availability(std::string_view command) const {
    return state_->availability(command);
}
std::optional<FilesAppTarget> FilesApp::target(std::size_t pane) const {
    const auto &p = state_->panes.at(pane);
    if (state_->closing || state_->volume_pending(pane) || p.refresh_failed || p.navigating || p.changing ||
        p.queued_view || p.queued_refresh || p.queued_copy.has_value() || p.inspection || p.mutating ||
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
