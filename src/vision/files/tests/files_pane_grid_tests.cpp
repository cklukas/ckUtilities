// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_pane_grid.hpp"
#include "native_index_file.hpp"
#include "ck/vision/suite_shell.hpp"
#include "scratch_directory.hpp"
#include "simulated_filesystem.hpp"
#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>
#include <cvision/widgets/splitter.hpp>
#include <cvision/testing/cktest.hpp>
#include <limits>
#include <array>
namespace {
using namespace ck::files;
using ck::vision::FilesPaneGridModel;
using ckv::widgets::GridMove;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("grid fixture: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
void load(PaneModel &pane, std::size_t count) {
    std::vector<PaneEntry> entries;
    for (std::size_t i = 0; i < count; ++i) {
        BasicMetadata metadata;
        metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        metadata.logical_size = Observation<std::uint64_t>::available(i);
        metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(-1, 999999999));
        entries.emplace_back(*NativeName::from_posix("file-" + std::to_string(i)), metadata,
                             Observation<bool>::available(false));
    }
    if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) != PanePublish::Applied)
        throw std::runtime_error("grid fixture publish");
}
std::shared_ptr<const PanePage> capture(PaneModel &pane, std::size_t first = 0, std::size_t count = 256) {
    auto result = capture_pane_page(pane, first, {count, 1024 * 1024});
    if (!result.page)
        throw std::runtime_error("grid fixture capture");
    return result.page;
}
} // namespace
CK_TEST(files_grid_displays_only_bounded_escaped_pages_and_never_interprets_names_as_targets) {
    PaneModel pane;
    std::vector<PaneEntry> entries;
    entries.emplace_back(*NativeName::from_posix("a\x1b[31m\xff"), BasicMetadata{}, Observation<bool>{});
    CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) == PanePublish::Applied);
    auto page = capture(pane);
    FilesPaneGridModel model;
    model.set_viewport(20, 100);
    CK_CHECK(model.publish(page) && model.row_count() == 1 && model.current());
    CK_CHECK(model.cell(0, 1).text == page->rows()[0].display_name);
    CK_CHECK(model.cell(0, 1).text.find('\x1b') == std::string::npos);
    CK_CHECK(model.cell(0, 2).text == "Unknown" && model.cell(0, 3).text == "Unknown");
    CK_CHECK(model.target()->row == page->rows()[0].id && model.target()->stamp.current());
    CK_CHECK(model.cell(1, 1).text.empty() && model.cell(0, 99).text.empty());
}
CK_TEST(files_grid_cursor_navigation_and_uncached_rows_never_fabricate_row_targets) {
    PaneModel pane;
    load(pane, 100);
    FilesPaneGridModel model;
    model.set_viewport(5, 80);
    CK_CHECK(model.publish(capture(pane, 0, 5)));
    model.navigate(GridMove::GridEnd, false);
    CK_CHECK(model.cursor().row == 99 && model.viewport().first == 95 && !model.target());
    CK_CHECK(model.cell(99, 1).text == "Loading...");
    CK_CHECK(model.publish(capture(pane, 95, 5)) && model.target());
    const auto selected = model.target()->row;
    CK_CHECK(selected == capture(pane, 95, 5)->rows()[4].id);
    model.navigate(GridMove::PageUp, true);
    CK_CHECK(model.cursor().row == 94 && !model.selection() && !model.target());
    model.navigate(GridMove::GridStart, false);
    CK_CHECK(model.cursor().row == 0 && model.viewport().first == 0);
    model.navigate(GridMove::Up, false);
    CK_CHECK(model.cursor().row == 0);
}
CK_TEST(files_grid_wheel_origin_survives_repeated_viewport_reads_and_does_not_move_cursor) {
    PaneModel pane;
    load(pane, 100);
    FilesPaneGridModel model;
    model.set_viewport(5, 80);
    CK_CHECK(model.publish(capture(pane)));
    model.scroll_by(20, 0);
    model.set_viewport(5, 80);
    CK_CHECK(model.viewport().first == 20 && model.cursor().row == 0 && model.frame().rows.front() == 20);
    CK_CHECK(model.publish(capture(pane, 20, 5)));
    CK_CHECK(model.viewport().first == 20 && model.cursor().row == 0);
    model.scroll_by(std::numeric_limits<int>::min(), 0);
    CK_CHECK(model.viewport().first == 0);
    model.scroll_by(std::numeric_limits<int>::max(), 0);
    CK_CHECK(model.viewport().first == 95);
    model.navigate(GridMove::Down, false);
    CK_CHECK(model.viewport().first == 1 && model.cursor().row == 1);
}
CK_TEST(files_grid_stale_pages_remain_readable_but_cannot_supply_targets_or_replace_current_data) {
    PaneModel pane;
    load(pane, 3);
    FilesPaneGridModel model;
    auto page = capture(pane);
    CK_CHECK(model.publish(page));
    const auto text = model.cell(0, 1).text;
    const auto request = pane.begin_listing(PaneListingPurpose::Refresh);
    CK_CHECK(!model.current() && !model.target() && model.cell(0, 1).text == text);
    CK_CHECK(!model.publish(page));
    CK_CHECK(!model.publish(std::shared_ptr<const PanePage>{}));
    CK_CHECK(!model.publish(std::shared_ptr<const IndexedPanePage>{}));
    CK_CHECK(pane.cancel_listing(request));
    CK_CHECK(model.publish(capture(pane)) && model.current());
    model.clear();
    CK_CHECK(!model.current() && !model.target() && model.row_count() == 0 && model.frame().rows.empty());
}
CK_TEST(files_grid_clamps_geometry_and_exposes_named_columns_at_supported_widths) {
    PaneModel pane;
    load(pane, 2);
    FilesPaneGridModel model;
    CK_CHECK(model.publish(capture(pane)));
    model.set_viewport(-9, -1);
    CK_CHECK(model.frame().rows.empty() && model.column_width(1) >= 1);
    CK_CHECK(model.viewport().rows == 1);
    model.set_viewport(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    CK_CHECK(model.viewport().rows == 4096 && model.frame().rows.size() == 2 && model.frame().columns.size() == 5);
    for (int width : {10, 28, 52, 75, 120}) {
        model.set_viewport(3, width);
        const auto frame = model.frame();
        int used = 0;
        for (auto col : frame.columns)
            used += model.column_width(col) + 1;
        CK_CHECK(used <= width || width < 5);
    }
    CK_CHECK(model.column_label(4) == "Modified (UTC)" && model.row_header_width() == 0);
}
CK_TEST(files_grid_displays_zero_and_uint64_sizes_and_utc_without_rounding_native_metadata) {
    PaneModel pane;
    load(pane, 2);
    FilesPaneGridModel model;
    auto page = capture(pane);
    CK_CHECK(model.publish(page));
    CK_CHECK(model.cell(0, 3).text == "0");
    CK_CHECK(model.cell(0, 4).text == "1969-12-31 23:59:59");
    CK_CHECK(page->rows()[0].metadata.modified.value()->nanoseconds() == 999999999);
    model.place_cursor({999, 99}, true);
    CK_CHECK(model.cursor().row == 0);
    model.place_cursor({1, 4}, true);
    CK_CHECK(model.cursor().row == 1 && model.cursor().column == 1);
    model.navigate(GridMove::Left, false);
    model.navigate(GridMove::RowEnd, false);
    CK_CHECK(model.cursor().column == 1 && !model.selection());
}
CK_TEST(files_grid_indexed_native_cache_page_survives_worker_retirement_without_native_owners) {
    ck::testing::ScratchDirectory scratch("files-grid-index", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#ifndef _WIN32
    std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
#endif
    FilesPaneGridModel model;
    model.set_viewport(10, 80);
    {
        ck::testing::SimulatedFileSystem fs;
        auto root = fs.make_node(ObjectKind::Directory);
#ifdef _WIN32
        const std::filesystem::path path("V:/files-grid");
#else
        const std::filesystem::path path("/files-grid");
#endif
        fs.bind(path, root);
        for (auto name : {"z", "a", "b"})
            fs.put(root, *NativeName::from_posix(name), fs.make_node(ObjectKind::RegularFile));
        auto factory = take(open_native_index_workspace(take(NativeAbsolutePath::from_host(scratch.path()))));
        auto budget = std::make_shared<IndexStorageBudget>(4 * 1024 * 1024, 32);
        IndexedPane pane;
        auto request = pane.begin_listing(PaneOrder{});
        std::array<std::byte, 16> generation{};
        generation[0] = std::byte{1};
        auto listing =
            std::make_unique<DirectoryListing>(take(fs.open_directory(path)), DirectoryListingLimits{}, fs.clock());
        DirectoryIndexListingJob collect(request, std::move(listing), StorageModel::LinuxStatx, factory, budget,
                                         generation);
        for (int i = 0; i < 1000 && !collect.completed().value(); ++i)
            collect.advance();
        IndexedPaneDataJob prepare(take(collect.completed()), PaneOrder{});
        for (int i = 0; i < 1000 && !prepare.completed().value(); ++i)
            prepare.advance();
        CK_CHECK(pane.publish(take(prepare.completed())) == IndexedPanePublish::Applied);
        auto page = pane.capture_page(pane.state_stamp(), 0);
        CK_CHECK(page.page && model.publish(page.page));
        CK_CHECK(model.cell(0, 1).text == "a" && model.current());
    }
    CK_CHECK(model.cell(0, 1).text == "a" && !model.current() && !model.target());
    CK_CHECK(std::filesystem::is_empty(scratch.path()));
}
CK_TEST(files_grid_real_ckvision_splitter_renders_two_independent_bounded_models) {
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34});
    ckv::ui::Application application(terminal, clock);
    ck::vision::SuiteShell shell(application, {.application_name = "Files"});
    PaneModel left, right;
    load(left, 4);
    load(right, 7);
    FilesPaneGridModel lm, rm;
    CK_CHECK(lm.publish(capture(left)) && rm.publish(capture(right)));
    auto first = std::make_unique<ckv::widgets::CellGrid>();
    auto second = std::make_unique<ckv::widgets::CellGrid>();
    auto *lp = first.get();
    auto *rp = second.get();
    lp->set_model(lm);
    rp->set_model(rm);
    auto split =
        std::make_unique<ckv::widgets::Splitter>(ckv::Rect{0, 0, 120, 34}, std::move(first), std::move(second));
    auto *view = split.get();
    auto window = std::make_unique<ckv::widgets::Window>("Files");
    window->set_bounds(shell.desktop().content_area());
    window->set_content(std::move(split));
    shell.desktop().add_window(std::move(window));
    application.step(0);
    std::string painted;
    const auto frame = application.current_frame();
    for (int y = 0; y < frame.size().height; ++y) {
        for (int x = 0; x < frame.size().width; ++x)
            painted += frame.at({x, y}).grapheme();
        painted += '\n';
    }
    CK_CHECK(painted.find("file-0") != std::string::npos && painted.find("Name") != std::string::npos);
    CK_CHECK(lm.row_count() == 4 && rm.row_count() == 7 && application.current_frame().size() == ckv::Size(120, 34));
    lm.navigate(GridMove::Down, false);
    lp->model_changed();
    application.step(0);
    CK_CHECK(lm.cursor().row == 1 && rm.cursor().row == 0);
    view->first()->set_visible(false);
    application.step(0);
    CK_CHECK(view->second()->bounds().width == view->bounds().width && rm.row_count() == 7);
    lp->clear_model();
    rp->clear_model(); // Borrowers detach before providers retire.
}
CK_TEST(files_grid_native_value_boundaries_and_kinds_remain_explicit) {
    for (auto kind : {ObjectKind::RegularFile, ObjectKind::Directory, ObjectKind::SymbolicLink, ObjectKind::Junction,
                      ObjectKind::ReparsePoint, ObjectKind::Fifo, ObjectKind::Socket, ObjectKind::CharacterDevice,
                      ObjectKind::BlockDevice, ObjectKind::Other}) {
        for (auto seconds : std::array<std::int64_t, 4>{std::numeric_limits<std::int64_t>::min(), -62167219200LL,
                                                        253402300799LL, std::numeric_limits<std::int64_t>::max()}) {
            PaneModel pane;
            BasicMetadata metadata;
            metadata.kind = Observation<ObjectKind>::available(kind);
            metadata.logical_size = Observation<std::uint64_t>::available(std::numeric_limits<std::uint64_t>::max());
            metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(seconds, 1));
            std::vector<PaneEntry> entries;
            entries.emplace_back(*NativeName::from_posix("entry"), metadata, Observation<bool>{});
            CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) == PanePublish::Applied);
            FilesPaneGridModel model;
            CK_CHECK(model.publish(capture(pane)));
            CK_CHECK(model.cell(0, 3).text == "18446744073709551615" && model.cell(0, 2).text != "Unknown");
            const auto text = model.cell(0, 4).text;
            if (seconds < -62167219200LL || seconds > 253402300799LL)
                CK_CHECK(text == "Outside display range");
            else
                CK_CHECK(text == (seconds < 0 ? "0000-01-01 00:00:00" : "9999-12-31 23:59:59"));
        }
    }
}
CK_TEST(files_grid_worker_focus_is_followed_only_by_explicit_controller_choice) {
    PaneModel pane;
    load(pane, 5);
    FilesPaneGridModel model;
    model.set_viewport(2, 80);
    CK_CHECK(model.publish(capture(pane)));
    const auto id = capture(pane)->rows()[4].id;
    CK_CHECK(pane.focus(id) && pane.set_mark(id, true));
    auto page = capture(pane);
    CK_CHECK(model.publish(page) && model.cursor().row == 0 && model.cell(4, 0).text == "*");
    CK_CHECK(model.publish(page, true) && model.cursor().row == 4 && model.viewport().first == 3);
    model.navigate(GridMove::PageUp, true);
    CK_CHECK(model.cursor().row == 2 && model.cell(4, 0).text == "*" && !model.selection());
}
CK_TEST(files_grid_empty_capture_and_zero_geometry_have_no_synthetic_selection) {
    PaneModel pane;
    load(pane, 0);
    FilesPaneGridModel model;
    CK_CHECK(model.publish(capture(pane)));
    for (auto move : {GridMove::Up, GridMove::Down, GridMove::PageDown, GridMove::GridEnd})
        model.navigate(move, true);
    model.scroll_by(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    CK_CHECK(model.row_count() == 0 && model.frame().rows.empty() && !model.target() && model.cursor().row == 0);
}
