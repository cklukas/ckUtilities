// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_page.hpp"
#include <algorithm>
#include <stdexcept>

namespace ck::files {
namespace {
bool charge(std::size_t bytes, std::size_t& used, std::size_t limit) {
    if (bytes > limit - used) return false;
    used += bytes; return true;
}
template<class T> bool charge(const Observation<T>& value, std::size_t& used, std::size_t limit) {
    const auto* detail = value.unavailable_detail();
    return !detail || charge(detail->error.operation.size(), used, limit);
}
bool charge(const BasicMetadata& value, std::size_t& used, std::size_t limit) {
    return charge(value.identity, used, limit) && charge(value.kind, used, limit) &&
        charge(value.logical_size, used, limit) && charge(value.allocated_size, used, limit) &&
        charge(value.link_count, used, limit) && charge(value.posix_uid, used, limit) &&
        charge(value.posix_gid, used, limit) && charge(value.posix_permissions, used, limit) &&
        charge(value.bsd_flags, used, limit) && charge(value.windows_attributes, used, limit) &&
        charge(value.reparse_tag, used, limit) && charge(value.linux_attributes, used, limit) &&
        charge(value.mount_id, used, limit) && charge(value.accessed, used, limit) &&
        charge(value.modified, used, limit) && charge(value.changed, used, limit) && charge(value.created, used, limit);
}
bool charge(const std::optional<UnavailableObservation>& value, std::size_t& used, std::size_t limit) {
    return !value || charge(value->error.operation.size(), used, limit);
}
}
PanePageCapture capture_pane_page(const PaneModel& model, std::size_t first, PanePageLimits limits) {
    if (!limits.rows || limits.rows > 4096 || !limits.text_bytes || limits.text_bytes > 16 * 1024 * 1024)
        throw std::invalid_argument("invalid pane page limits");
    if (first > model.visible_size()) return {PanePageState::OutOfRange, {}};
    std::size_t used = 0;
    if (!charge(model.view().display_substring.size(), used, limits.text_bytes) ||
        !charge(model.listing_error(), used, limits.text_bytes)) return {PanePageState::LimitExceeded, {}};
    const auto count = std::min(limits.rows, model.visible_size() - first);
    // Check all dynamic payloads before copying. Traversal is bounded by the
    // requested page size, regardless of directory size or number of marks.
    for (std::size_t i = 0; i < count; ++i) {
        const auto& row = *model.visible_row(first + i);
        if (!charge(row.display_name.size(), used, limits.text_bytes) ||
            !charge(row.entry.metadata, used, limits.text_bytes) ||
            !charge(row.entry.hidden, used, limits.text_bytes) ||
            !charge(row.entry.inspection_error(), used, limits.text_bytes)) return {PanePageState::LimitExceeded, {}};
    }
    auto page = std::shared_ptr<PanePage>(new PanePage);
    page->first_ = first; page->total_rows_ = model.size(); page->visible_rows_ = model.visible_size();
    page->text_bytes_ = used; page->view_ = model.view(); page->pending_ = model.listing_pending();
    page->selection_ = model.selection_summary();
    page->error_ = model.listing_error(); page->invalidated_ = model.invalidated_selection();
    page->rows_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& row = *model.visible_row(first + i);
        page->rows_.push_back({row.id, row.display_name, row.entry.metadata, row.entry.hidden,
            row.entry.inspection_error(), model.marked(row.id), model.focused() == row.id});
    }
    page->stamp_ = model.state_stamp();
    return {PanePageState::Ready, std::move(page)};
}
} // namespace ck::files
