// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_pane_grid.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
namespace ck::vision {
namespace {
std::string kind_text(const ck::files::Observation<ck::files::ObjectKind> &kind) {
    if (!kind.value())
        return "Unknown";
    using K = ck::files::ObjectKind;
    switch (*kind.value()) {
    case K::RegularFile:
        return "File";
    case K::Directory:
        return "Folder";
    case K::SymbolicLink:
        return "Link";
    case K::Junction:
        return "Junction";
    case K::ReparsePoint:
        return "Reparse";
    case K::Fifo:
        return "FIFO";
    case K::Socket:
        return "Socket";
    case K::CharacterDevice:
        return "Char device";
    case K::BlockDevice:
        return "Block device";
    case K::Other:
        return "Other";
    }
    return "Unknown";
}
std::string time_text(const ck::files::Observation<ck::files::FileTimestamp> &time) {
    if (!time.value())
        return "Unknown";
    const auto value = *time.value();
    // Limit calendar conversion to the explicitly displayed four-digit year.
    if (value.seconds() < -62167219200LL || value.seconds() > 253402300799LL)
        return "Outside display range";
    const std::chrono::sys_seconds point{std::chrono::seconds(value.seconds())};
    const auto day = std::chrono::floor<std::chrono::days>(point);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss clock{point - day};
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-' << std::setw(2)
        << static_cast<unsigned>(date.month()) << '-' << std::setw(2) << static_cast<unsigned>(date.day()) << ' '
        << std::setw(2) << clock.hours().count() << ':' << std::setw(2) << clock.minutes().count() << ':'
        << std::setw(2) << clock.seconds().count();
    return out.str(); // Display seconds only; no change to retained native precision.
}
std::uint64_t shift(std::uint64_t at, int delta, std::uint64_t maximum) {
    if (delta >= 0)
        return at + std::min<std::uint64_t>(static_cast<unsigned>(delta), maximum - at);
    const auto magnitude = static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta));
    return at - std::min(at, magnitude);
}
} // namespace
const std::vector<ck::files::PaneDisplayRow> *FilesPaneGridModel::rows() const noexcept {
    return std::visit(
        [](const auto &page) -> const std::vector<ck::files::PaneDisplayRow> * {
            if constexpr (requires { page->rows(); })
                return &page->rows();
            else
                return nullptr;
        },
        page_);
}
const ck::files::PaneStateStamp *FilesPaneGridModel::stamp() const noexcept {
    return std::visit(
        [](const auto &page) -> const ck::files::PaneStateStamp * {
            if constexpr (requires { page->stamp(); })
                return &page->stamp();
            else
                return nullptr;
        },
        page_);
}
bool FilesPaneGridModel::current() const noexcept { return stamp() && stamp()->current(); }
std::uint64_t FilesPaneGridModel::row_count() const noexcept { return total_; }
FilesPaneViewport FilesPaneGridModel::viewport() const noexcept {
    return {first_, static_cast<std::size_t>(std::max(1, height_))};
}
template <class P> bool FilesPaneGridModel::adopt(std::shared_ptr<const P> page, bool follow_worker_focus) {
    if (!page || !page->stamp().current() || page->rows().size() > static_cast<std::size_t>(max_viewport_rows) ||
        page->visible_rows() > std::numeric_limits<std::size_t>::max())
        return false;
    // Sealed producers validate row IDs, text budgets and complete capture.
    total_ = page->visible_rows();
    page_first_ = page->first();
    page_ = std::move(page);
    first_ = std::min(first_, last_origin());
    cursor_ = static_cast<std::size_t>(std::min<std::uint64_t>(cursor_, total_ ? total_ - 1 : 0));
    const auto *values = rows();
    if (follow_worker_focus) {
        for (std::size_t i = 0; i < values->size(); ++i)
            if ((*values)[i].focused) {
                cursor_ = static_cast<std::size_t>(page_first_ + i);
                break;
            }
        reveal();
    }
    return true;
}
bool FilesPaneGridModel::publish(std::shared_ptr<const ck::files::PanePage> page, bool follow_worker_focus) {
    return adopt(std::move(page), follow_worker_focus);
}
bool FilesPaneGridModel::publish(std::shared_ptr<const ck::files::IndexedPanePage> page, bool follow_worker_focus) {
    return adopt(std::move(page), follow_worker_focus);
}
void FilesPaneGridModel::clear() noexcept {
    page_ = std::monostate{};
    total_ = page_first_ = first_ = 0;
    cursor_ = 0;
    input_locked_ = false;
}
const ck::files::PaneDisplayRow *FilesPaneGridModel::row(std::uint64_t at) const noexcept {
    const auto *values = rows();
    if (!values || at < page_first_ || at - page_first_ >= values->size())
        return nullptr;
    return &(*values)[static_cast<std::size_t>(at - page_first_)];
}
std::optional<FilesPaneRowTarget> FilesPaneGridModel::target() const {
    const auto *value = row(cursor_);
    if (!current() || !value)
        return {};
    return FilesPaneRowTarget{*stamp(), value->id};
}
const ck::files::PaneDisplayRow *FilesPaneGridModel::cursor_row() const noexcept {
    return current() ? row(cursor_) : nullptr;
}
std::optional<bool> FilesPaneGridModel::cursor_marked() const {
    const auto *value = row(cursor_);
    return current() && value ? std::optional<bool>(value->marked) : std::nullopt;
}
std::optional<ck::files::ObjectKind> FilesPaneGridModel::cursor_kind() const {
    const auto *value = row(cursor_);
    return current() && value && value->metadata.kind.value()
               ? std::optional<ck::files::ObjectKind>(*value->metadata.kind.value())
               : std::nullopt;
}
void FilesPaneGridModel::set_viewport(int rows_count, int width) {
    const auto previous_height = height_;
    height_ = std::clamp(rows_count, 0, max_viewport_rows);
    width_ = std::clamp(width, 0, 65536);
    if (height_ != previous_height)
        first_ = std::min(first_, last_origin());
}
ckv::widgets::GridFrame FilesPaneGridModel::frame() const {
    ckv::widgets::GridFrame result;
    const auto count = std::min<std::uint64_t>(static_cast<unsigned>(height_), total_ - first_);
    result.rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i)
        result.rows.push_back(static_cast<std::size_t>(first_ + i));
    result.columns = {0, 1};
    if (width_ >= 52)
        result.columns.push_back(2);
    if (width_ >= 28)
        result.columns.push_back(3);
    if (width_ >= 75)
        result.columns.push_back(4);
    return result;
}
int FilesPaneGridModel::column_width(ckv::widgets::GridIndex column) const {
    switch (column) {
    case 0:
        return 2;
    case 1:
        return std::max(1, width_ - 4 - (width_ >= 52 ? 13 : 0) - (width_ >= 28 ? 13 : 0) - (width_ >= 75 ? 20 : 0));
    case 2:
        return 12;
    case 3:
        return 12;
    case 4:
        return 19;
    default:
        return 1;
    }
}
std::string FilesPaneGridModel::column_label(ckv::widgets::GridIndex column) const {
    switch (column) {
    case 0:
        return "M";
    case 1:
        return "Name";
    case 2:
        return "Type";
    case 3:
        return "Bytes";
    case 4:
        return "Modified (UTC)";
    default:
        return {};
    }
}
ckv::widgets::GridCell FilesPaneGridModel::cell(ckv::widgets::GridIndex at, ckv::widgets::GridIndex column) const {
    ckv::widgets::GridCell result;
    if (at >= total_)
        return result;
    const auto *value = row(at);
    if (!value) {
        if (column == 1)
            result.text = "Loading...";
        return result;
    }
    switch (column) {
    case 0:
        result.text = value->marked ? "*" : "";
        break;
    case 1:
        result.text = value->display_name;
        break;
    case 2:
        result.text = kind_text(value->metadata.kind);
        break;
    case 3:
        result.text =
            value->metadata.logical_size.value() ? std::to_string(*value->metadata.logical_size.value()) : "Unknown";
        result.alignment = ckv::widgets::CellAlignment::End;
        break;
    case 4:
        result.text = time_text(value->metadata.modified);
        break;
    default:
        break;
    }
    return result;
}
std::uint64_t FilesPaneGridModel::last_origin() const noexcept {
    return total_ - std::min<std::uint64_t>(total_, static_cast<unsigned>(std::max(1, height_)));
}
void FilesPaneGridModel::reveal() noexcept {
    if (!height_ || !total_)
        return;
    if (cursor_ < first_)
        first_ = cursor_;
    else if (cursor_ - first_ >= static_cast<unsigned>(height_))
        first_ = cursor_ - static_cast<unsigned>(height_) + 1;
}
void FilesPaneGridModel::navigate(ckv::widgets::GridMove move, bool) {
    if (input_locked_ || !total_)
        return;
    using M = ckv::widgets::GridMove;
    auto next = static_cast<std::uint64_t>(cursor_);
    switch (move) {
    case M::Up:
        next = shift(next, -1, total_ - 1);
        break;
    case M::Down:
        next = shift(next, 1, total_ - 1);
        break;
    case M::PageUp:
        next = shift(next, -std::max(1, height_), total_ - 1);
        break;
    case M::PageDown:
        next = shift(next, std::max(1, height_), total_ - 1);
        break;
    case M::JumpUp:
    case M::GridStart:
    case M::RowStart: // Home in a file list addresses the first record.
        next = 0;
        break;
    case M::JumpDown:
    case M::GridEnd:
    case M::RowEnd: // End addresses the last record, not a metadata column.
        next = total_ - 1;
        break;
    case M::Left:
    case M::Right:
    case M::JumpLeft:
    case M::JumpRight:
        return;
    }
    cursor_ = static_cast<std::size_t>(next);
    reveal();
}
void FilesPaneGridModel::place_cursor(ckv::widgets::GridPosition at, bool) {
    if (input_locked_ || !total_ || at.row >= total_)
        return;
    cursor_ = at.row;
    reveal();
}
void FilesPaneGridModel::scroll_by(int delta, int) {
    if (!input_locked_ && total_)
        first_ = shift(first_, delta, last_origin());
}
} // namespace ck::vision
