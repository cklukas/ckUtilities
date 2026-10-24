// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "indexed_pane.hpp"
#include <cvision/widgets/cell_grid.hpp>
#include <variant>
namespace ck::vision {
struct FilesPaneViewport {
    std::uint64_t first = 0;
    std::size_t rows = 1;
    friend bool operator==(const FilesPaneViewport &, const FilesPaneViewport &) = default;
};
struct FilesPaneRowTarget {
    ck::files::PaneStateStamp stamp;
    ck::files::PaneRowId row;
};
// UI-thread projection only. Owns at most one bounded immutable display page,
// never a native name, worker model, index file, directory cursor or HeldChild.
// Provider calls perform no I/O, scheduling, callbacks or whole-directory scans.
// A controller gates asynchronous delivery before publish(), observes viewport()
// outside painting, requests pages on a worker and calls grid.model_changed().
// Positions address display pages only; target() returns a stamped row ID, never
// operation permission. The receiving worker must validate that stamp again.
class FilesPaneGridModel final : public ckv::widgets::CellGridModel {
  public:
    static constexpr int max_viewport_rows = 4096;
    bool publish(std::shared_ptr<const ck::files::PanePage> page, bool follow_worker_focus = false);
    bool publish(std::shared_ptr<const ck::files::IndexedPanePage> page, bool follow_worker_focus = false);
    void clear() noexcept;
    FilesPaneViewport viewport() const noexcept;
    std::uint64_t row_count() const noexcept;
    bool current() const noexcept;
    std::optional<FilesPaneRowTarget> target() const;
    const ck::files::PaneDisplayRow *cursor_row() const noexcept;
    std::optional<bool> cursor_marked() const;
    std::optional<ck::files::ObjectKind> cursor_kind() const;
    // Acknowledged state changes pause input without dropping focus or hiding
    // the retained page. Geometry updates and rendering remain available.
    void set_input_locked(bool locked) noexcept { input_locked_ = locked; }
    void set_viewport(int rows, int width) override;
    ckv::widgets::GridFrame frame() const override;
    int column_width(ckv::widgets::GridIndex column) const override;
    int row_header_width() const override { return 0; }
    std::string row_label(ckv::widgets::GridIndex) const override { return {}; }
    std::string column_label(ckv::widgets::GridIndex column) const override;
    ckv::widgets::GridCell cell(ckv::widgets::GridIndex row, ckv::widgets::GridIndex column) const override;
    ckv::widgets::GridPosition cursor() const override { return {cursor_, 1}; }
    std::optional<ckv::widgets::GridRange> selection() const override { return {}; }
    // Range marking is a worker command, not a local rectangle or cached flag.
    // Shift movement moves the cursor only; it never silently marks files.
    void navigate(ckv::widgets::GridMove move, bool extend) override;
    void place_cursor(ckv::widgets::GridPosition at, bool extend) override;
    void scroll_by(int rows, int columns) override;

  private:
    using Page = std::variant<std::monostate, std::shared_ptr<const ck::files::PanePage>,
                              std::shared_ptr<const ck::files::IndexedPanePage>>;
    template <class P> bool adopt(std::shared_ptr<const P> page, bool follow_worker_focus);
    std::uint64_t last_origin() const noexcept;
    const std::vector<ck::files::PaneDisplayRow> *rows() const noexcept;
    const ck::files::PaneStateStamp *stamp() const noexcept;
    const ck::files::PaneDisplayRow *row(std::uint64_t at) const noexcept;
    void reveal() noexcept;
    Page page_;
    std::uint64_t total_ = 0, page_first_ = 0, first_ = 0;
    std::size_t cursor_ = 0;
    int height_ = 0, width_ = 0;
    bool input_locked_ = false;
};
} // namespace ck::vision
