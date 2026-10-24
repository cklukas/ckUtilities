// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pane_model.hpp"

namespace ck::files {
struct PanePageLimits {
    std::size_t rows = 256; // hard maximum 4096
    std::size_t text_bytes = 1024 * 1024; // hard maximum 16 MiB
};
struct PaneDisplayRow {
    PaneRowId id;
    std::string display_name;
    BasicMetadata metadata;
    Observation<bool> hidden;
    std::optional<UnavailableObservation> inspection_error;
    bool marked = false;
    bool focused = false;
};
struct PanePageCapture;
// Capture runs entirely on the model's serialized worker. Published pages
// contain no model, cursor, HeldChild, raw native name or native owner. The UI
// may retain/read a const page after that worker retires the entire model.
class PanePage final {
public:
    const PaneStateStamp& stamp() const noexcept { return stamp_; }
    const std::vector<PaneDisplayRow>& rows() const noexcept { return rows_; }
    std::size_t first() const noexcept { return first_; }
    std::size_t total_rows() const noexcept { return total_rows_; }
    std::size_t visible_rows() const noexcept { return visible_rows_; }
    std::size_t text_bytes() const noexcept { return text_bytes_; }
    const PaneView& view() const noexcept { return view_; }
    bool listing_pending() const noexcept { return pending_; }
    const std::optional<UnavailableObservation>& listing_error() const noexcept { return error_; }
    const PaneInvalidatedSelection& invalidated_selection() const noexcept { return invalidated_; }
private:
    PanePage() = default;
    PaneStateStamp stamp_;
    std::vector<PaneDisplayRow> rows_;
    std::size_t first_ = 0, total_rows_ = 0, visible_rows_ = 0, text_bytes_ = 0;
    PaneView view_;
    bool pending_ = false;
    std::optional<UnavailableObservation> error_;
    PaneInvalidatedSelection invalidated_;
    friend PanePageCapture capture_pane_page(const PaneModel&, std::size_t, PanePageLimits);
};
enum class PanePageState { Ready, OutOfRange, LimitExceeded };
struct PanePageCapture {
    PanePageState state;
    std::shared_ptr<const PanePage> page; // present only for Ready; never partial
};
// Fixed object/row counts bound overhead separately from the aggregate copied
// string bytes (display names, filter, every retained diagnostic operation).
// Invalid limits throw; an oversized page fails without truncating any field.
// No I/O, whole-directory scan, sorting, native decoding or name/path lookup.
PanePageCapture capture_pane_page(const PaneModel& model, std::size_t first, PanePageLimits limits = {});
} // namespace ck::files
