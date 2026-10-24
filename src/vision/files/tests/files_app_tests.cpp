// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "scratch_directory.hpp"
#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/testing/cktest.hpp>
#include <cvision/widgets/dialog.hpp>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
namespace {
using namespace ck::vision;
using namespace ck::files;
template <class T> void find_views(ckv::ui::View &root, std::vector<T *> &found) {
    if (auto *view = dynamic_cast<T *>(&root))
        found.push_back(view);
    for (const auto &child : root.children())
        find_views(*child, found);
}
NativeAbsolutePath address(const std::filesystem::path &path) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value())
        throw std::runtime_error("invalid app fixture address");
    return std::move(value).take_value();
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void wait() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }))
            throw std::runtime_error("blocked app worker did not enter gate");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct Fixture {
    ck::testing::ScratchDirectory scratch{"files-app", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT};
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal{ckv::Size{120, 34}};
    ckv::ui::Application application{terminal, clock};
    FilesAppWake wake{application};
    std::shared_ptr<Gate> gate;
    std::array<std::unique_ptr<FilesPaneService>, 2> services;
    std::unique_ptr<FilesApp> app;
    unsigned exits = 0;
    explicit Fixture(ThemeScheme theme = ThemeScheme::Classic, std::shared_ptr<Gate> blocked = {},
                     FilesPaneServiceLimits limits = {}, ckv::Size size = {120, 34},
                     std::shared_ptr<std::atomic<bool>> fail_notification = {})
        : terminal(size), gate(std::move(blocked)) {
#ifndef _WIN32
        std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
        for (std::size_t i = 0; i < 2; ++i) {
            const auto dir = scratch.path() / (i == 0 ? "left" : "right");
            const auto cache = scratch.path() / (i == 0 ? "cache-left" : "cache-right");
            std::filesystem::create_directory(dir);
            std::filesystem::create_directory(cache);
#ifndef _WIN32
            std::filesystem::permissions(cache, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace);
#endif
            for (int n = 0; n < (i == 0 ? 100 : 3); ++n) {
                std::ofstream file(dir / ((i == 0 ? "left-item-" : "right-item-") + std::to_string(n)));
                file << n;
                if (!file)
                    throw std::runtime_error("cannot write app fixture");
            }
            auto backend = native_files_pane_backend(address(cache));
            if (i == 0 && gate) {
                backend.filesystem = [create = std::move(backend.filesystem), hold = gate] {
                    hold->block();
                    return create();
                };
            }
            auto notify = wake.callback();
            if (i == 0 && fail_notification)
                notify = [signal = std::move(notify), fault = fail_notification] {
                    if (fault->exchange(false))
                        throw std::runtime_error("injected acknowledgment notification failure");
                    signal();
                };
            services[i] = std::make_unique<FilesPaneService>(std::move(backend), limits, std::move(notify));
        }
        app = std::make_unique<FilesApp>(
            application, std::array<FilesPaneService *, 2>{services[0].get(), services[1].get()}, wake,
            std::array<NativeAbsolutePath, 2>{address(scratch.path() / "left"), address(scratch.path() / "right")},
            [this] { ++exits; }, theme);
    }
    ~Fixture() {
        app.reset();
        if (gate)
            gate->release();
        for (auto &service : services)
            service->join();
        application.step(0); // Drain late weak posts after controller destruction.
    }
    template <class Predicate> void until(Predicate ready) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            application.step(clock.now_nanos());
            if (ready())
                return;
            clock.advance(1'000'000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < end);
        throw std::runtime_error("Files application condition timed out: " + app->status(0) + " / " + app->status(1));
    }
    void loaded() {
        until([&] { return !app->pending(0) && !app->pending(1); });
    }
    ckv::ui::CommandId id(std::string_view name) {
        const auto value = application.commands().id_for(name);
        if (!value)
            throw std::runtime_error("missing Files test command");
        return *value;
    }
    bool command(std::string_view name) { return application.execute_command(id(name)); }
    bool key(ckv::Key value, ckv::Modifier modifiers = ckv::Modifier::None) {
        return application.dispatch(ckv::KeyEvent{{value, modifiers, {}}});
    }
    std::string painted() {
        application.step(clock.now_nanos());
        std::string text;
        const auto &frame = application.current_frame();
        for (int y = 0; y < frame.size().height; ++y) {
            for (int x = 0; x < frame.size().width; ++x)
                text += frame.at({x, y}).grapheme();
            text += '\n';
        }
        return text;
    }
};
} // namespace
CK_TEST(files_app_composes_independent_native_panes_in_every_suite_theme) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        CK_CHECK(f.app->model(0).row_count() == 100 && f.app->model(1).row_count() == 3);
        CK_CHECK(f.app->target(0) && f.app->target(1) && !f.app->narrow());
        const auto text = f.painted();
        CK_CHECK(text.find("left-item-") != text.npos && text.find("right-item-") != text.npos);
        CK_CHECK(text.find("[Active] Left") != text.npos && text.find("[Other] Right") != text.npos);
        CK_CHECK(f.app->status(0) == "Last scan: 100 items");
    }
}
CK_TEST(files_app_narrow_layout_switches_without_discarding_either_pane) {
    Fixture f;
    f.loaded();
    const auto left = f.app->target(0)->row.row;
    const auto right = f.app->target(1)->row.row;
    f.terminal.resize({80, 24});
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->narrow() && f.app->grid(0)->bounds().width > 70);
    CK_CHECK(f.application.execute_command(f.app->switch_command()));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->active_pane() == 1 && f.application.focused() == f.app->grid(1));
    CK_CHECK(f.app->target(0)->row.row == left && f.app->target(1)->row.row == right);
    const auto text = f.painted();
    CK_CHECK(text.find("[Active] Right") != text.npos && text.find("Other: Left") != text.npos);
    f.terminal.resize({120, 34});
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->narrow() && f.app->active_pane() == 1);
}
CK_TEST(files_app_failed_navigation_retains_previous_rows_and_reports_failure) {
    Fixture f;
    f.loaded();
    const auto old = f.app->model(0).cell(0, 1).text;
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "missing")));
    CK_CHECK(!f.app->target(0));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 100 && f.app->model(0).cell(0, 1).text == old);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
    CK_CHECK(f.app->model(1).row_count() == 3 && f.app->target(1));
}
CK_TEST(files_app_keyboard_pages_beyond_initial_cache_and_keeps_other_cursor_independent) {
    Fixture f;
    f.loaded();
    const auto other = f.app->target(1)->row.row;
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::End, ckv::Modifier::Ctrl, {}}}));
    CK_CHECK(f.app->model(0).cursor().row == 99 && !f.app->target(0));
    f.until([&] { return !f.app->pending(0) && f.app->target(0).has_value(); });
    CK_CHECK(f.app->model(0).cell(99, 1).text != "Loading...");
    CK_CHECK(f.app->target(1)->row.row == other && f.app->model(1).cursor().row == 0);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::F4, ckv::Modifier::None, {}}}));
    CK_CHECK(f.app->active_pane() == 1 && f.application.focused() == f.app->grid(1));
    CK_CHECK(f.app->model(0).cursor().row == 99);
}
CK_TEST(files_app_rapid_navigation_publishes_only_the_latest_requested_location) {
    Fixture f;
    for (int i = 0; i < 20; ++i)
        CK_CHECK(f.app->navigate(0, address(f.scratch.path() / (i % 2 == 0 ? "missing" : "left"))));
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 3 && f.app->target(0));
    CK_CHECK(f.app->model(0).cell(0, 1).text == "right-item-0");
    CK_CHECK(f.app->status(0) == "Last scan: 3 items");
}
CK_TEST(files_app_mark_controls_acknowledge_changes_and_preserve_the_other_pane) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.key(ckv::Key::Insert));
    CK_CHECK(f.app->pending(0) && !f.app->target(0));
    CK_CHECK(!f.command("ck.files.toggle_mark"));
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(0).cursor().row == 0); // Input pauses while marks commit.
    CK_CHECK(f.command("ck.files.switch_pane") && f.app->active_pane() == 1);
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(1).cursor().row == 1);
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->model(0).cursor_marked() == true);
    CK_CHECK(f.app->selection(1).marks->total == 0 && f.app->status(0).find("Marked: 1") != std::string::npos);
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->selection(1).marks->total == 3);
    CK_CHECK(f.command("ck.files.invert_marks"));
    f.loaded();
    CK_CHECK(f.app->selection(1).marks->total == 0 && f.app->selection(0).marks->total == 1);
}
CK_TEST(files_app_range_marks_span_uncached_rows_and_are_reset_by_a_view_change) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start"));
    CK_CHECK(f.key(ckv::Key::End, ckv::Modifier::Ctrl));
    f.until([&] { return !f.app->pending(0) && f.app->target(0); });
    CK_CHECK(f.command("ck.files.mark_range"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 100 && f.app->model(0).cursor_marked() == true);
    CK_CHECK(f.command("ck.files.unmark_range"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 0);
    auto view = f.app->view(0);
    view.descending = true;
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(!f.command("ck.files.mark_range") && f.app->status(0).find("Range start set") == std::string::npos);
}
CK_TEST(files_app_view_change_acknowledges_current_row_and_keeps_focus_and_marks_through_reorder) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        if (theme == ThemeScheme::Classic) {
            const auto cell = f.app->grid(0)->cell_rect({7, 1});
            if (!cell)
                throw std::runtime_error("missing real pane row cell");
            const auto grid = f.app->grid(0)->absolute_bounds();
            const ckv::Point at{grid.x + cell->x + cell->width / 2, grid.y + cell->y};
            CK_CHECK(f.application.dispatch(
                ckv::MouseEvent{.action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = at}));
            CK_CHECK(f.application.dispatch(
                ckv::MouseEvent{.action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = at}));
        } else
            for (unsigned i = 0; i < 7; ++i)
                CK_CHECK(f.key(ckv::Key::Down));
        const auto target = f.app->target(0)->row.row;
        CK_CHECK(f.command("ck.files.toggle_mark"));
        f.loaded();
        auto view = f.app->view(0);
        view.descending = true;
        CK_CHECK(f.app->change_view(0, view));
        CK_CHECK(f.app->pending(0) && !f.app->change_view(0, view) && !f.app->target(0));
        f.key(ckv::Key::Down);
        CK_CHECK(f.app->model(0).cursor().row == 7);
        f.loaded();
        CK_CHECK(f.app->view(0).descending && f.app->target(0)->row.row == target);
        CK_CHECK(f.app->model(0).cursor().row == 92 && f.app->model(0).cursor_marked() == true);
        CK_CHECK(f.app->selection(0).marks->total == 1 && !f.app->view(1).descending);
    }
}
CK_TEST(files_app_filtering_keeps_hidden_marks_explicit_and_clear_all_removes_them) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    auto view = f.app->view(0);
    view.display_substring = "left-item-0";
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 1 && f.app->selection(0).marks->hidden == 99);
    CK_CHECK(f.app->status(0).find("99 hidden; review required") != std::string::npos);
    CK_CHECK(f.command("ck.files.unmark_visible"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 99 && !*f.app->model(0).cursor_marked());
    CK_CHECK(f.command("ck.files.clear_marks"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.clear_marks"));
    view.display_substring = "no item matches";
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 0 && !f.command("ck.files.toggle_mark") &&
             !f.command("ck.files.mark_visible"));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    view.display_substring.clear();
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 100 && f.app->target(0));
}
CK_TEST(files_app_pane_commands_do_not_run_from_unrelated_input_or_view_options) {
    Fixture f;
    f.loaded();
    auto input = std::make_unique<ckv::widgets::InputLine>();
    input->set_bounds({0, 0, 20, 1});
    auto *outside = f.application.root().add_child(std::move(input));
    f.application.set_focus(outside);
    CK_CHECK(!f.command("ck.files.toggle_mark") && !f.command("ck.files.view_options"));
    CK_CHECK(!f.application.command_available(f.app->switch_command()));
    f.application.root().remove_child(outside);
    f.application.set_focus(f.app->grid(0));
    CK_CHECK(f.command("ck.files.view_options") && f.app->view_options_pending());
    CK_CHECK(!f.command("ck.files.toggle_mark") && !f.command("ck.files.mark_visible"));
    CK_CHECK(!f.command("ck.files.switch_pane"));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->view_options_pending() && !f.app->pending(0) && f.app->selection(0).marks->total == 0);
}
CK_TEST(files_app_view_options_materialize_typed_controls_and_accept_via_mouse_after_narrow_resize) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.view_options"));
    f.terminal.resize({80, 24});
    f.application.step(f.clock.now_nanos());
    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::CheckGroup *> checks;
    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), checks);
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(combos.size() == 1 && checks.size() == 4);
    combos.at(0)->set_selected_index(2); // Bytes, not a parsed display string.
    checks.at(0)->set_checked(0, true);
    checks.at(1)->set_checked(0, false);
    checks.at(2)->set_checked(0, true);
    checks.at(3)->set_checked(0, true);
    ckv::widgets::InputLine *filter = nullptr;
    for (auto *input : inputs) {
        bool in_combo = false;
        for (auto *parent = input->parent(); parent; parent = parent->parent())
            in_combo = in_combo || dynamic_cast<ckv::widgets::ComboBox *>(parent) != nullptr;
        if (!in_combo)
            filter = input;
    }
    if (!filter)
        throw std::runtime_error("missing real view filter input");
    filter->set_text("left-item-0");
    f.application.set_focus(filter);
    f.application.step(f.clock.now_nanos());
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply")
            apply = button;
    if (!apply)
        throw std::runtime_error("missing real view Apply button");
    const auto rect = apply->absolute_bounds();
    CK_CHECK(rect.width > 0 && rect.height > 0 && rect.y >= 0 && rect.y < 24 && rect.x >= 0 && rect.x < 80);
    const ckv::Point click{rect.x, rect.y};
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = click}));
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = click}));
    f.until([&] { return !f.app->view_options_pending() && !f.app->pending(0); });
    const auto &view = f.app->view(0);
    CK_CHECK(view.sort == PaneSortKey::Size && view.descending && !view.folders_first && view.show_hidden &&
             view.ascii_case_sensitive);
    CK_CHECK(view.display_substring == "left-item-0" && f.app->model(0).row_count() == 1);
}
CK_TEST(files_app_marks_menu_routes_context_commands_and_quit_cancels_view_options) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "m"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "m"}}));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 100);
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    f.until([&] { return f.exits == 1; });
    CK_CHECK(!f.app->view_options_pending() && !f.app->close_confirmation_pending());
    CK_CHECK(f.app->view(0).display_substring.empty());
}
CK_TEST(files_app_view_dialog_rejects_oversize_filters_and_obsolete_navigation_contexts) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.view_options"));
    std::vector<ckv::widgets::InputLine *> inputs;
    find_views(f.application.root(), inputs);
    ckv::widgets::InputLine *filter = nullptr;
    for (auto *input : inputs) {
        bool in_combo = false;
        for (auto *parent = input->parent(); parent; parent = parent->parent())
            in_combo = in_combo || dynamic_cast<ckv::widgets::ComboBox *>(parent) != nullptr;
        if (!in_combo)
            filter = input;
    }
    if (!filter)
        throw std::runtime_error("missing filter field");
    filter->set_text(std::string(4097, 'x'));
    f.application.set_focus(filter);
    CK_CHECK(f.key(ckv::Key::Enter));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->view_options_pending() && !f.app->pending(0));
    filter->set_text("right-item-0");
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.loaded();
    f.application.set_focus(filter);
    CK_CHECK(f.key(ckv::Key::Enter));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->view_options_pending() && !f.app->pending(0));
    CK_CHECK(f.app->model(0).row_count() == 3 && f.app->view(0).display_substring.empty());
    CK_CHECK(f.app->status(0).find("Location changed while view options were open") != std::string::npos);
}
CK_TEST(files_app_reports_committed_marks_when_display_capture_fails_and_recovers_after_resize) {
    FilesPaneServiceLimits limits;
    limits.page_wire_bytes = 4096;
    Fixture f(ThemeScheme::Classic, {}, limits, {120, 12});
    f.loaded();
    CK_CHECK(f.app->target(0));
    f.terminal.resize({120, 60});
    f.application.step(f.clock.now_nanos());
    f.loaded();
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->status(0).find("Change applied;") != std::string::npos && !f.app->target(0));
    CK_CHECK(!f.command("ck.files.toggle_mark"));
    f.terminal.resize({120, 12});
    f.application.step(f.clock.now_nanos());
    f.until([&] { return !f.app->pending(0) && f.app->target(0); });
    CK_CHECK(f.app->selection(0).marks->total == 100 && f.app->model(0).cursor_marked() == true);
    CK_CHECK(f.app->status(0).find("Change applied;") != std::string::npos);
}
CK_TEST(files_app_cancelling_an_acknowledged_focus_stage_does_not_start_its_queued_view) {
    Fixture f;
    f.loaded();
    for (unsigned i = 0; i < 7; ++i)
        f.key(ckv::Key::Down);
    auto view = f.app->view(0);
    view.descending = true;
    CK_CHECK(f.app->change_view(0, view));
    // Keep the UI's focus acknowledgment unread: cancel() must also withdraw
    // the queued view when the worker has already finished that first stage.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.services[0]->working()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("focus preparation did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(f.command("ck.files.cancel_read"));
    f.loaded();
    CK_CHECK(!f.app->view(0).descending && f.app->model(0).cursor().row == 7);
    CK_CHECK(f.app->status(0).find("View change cancelled before application") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(0).cursor().row == 8);
}
CK_TEST(files_app_unknown_change_acknowledgment_blocks_that_pane_and_keeps_the_other_usable) {
    auto fault = std::make_shared<std::atomic<bool>>(false);
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, fault);
    f.loaded();
    *fault = true;
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->status(0).find("Change outcome unknown;") != std::string::npos);
    CK_CHECK(!f.app->target(0) && !f.command("ck.files.toggle_mark") && !f.command("ck.files.view_options"));
    CK_CHECK(f.command("ck.files.switch_pane") && f.app->active_pane() == 1);
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->selection(1).marks->total == 3 && f.app->target(1));
}
CK_TEST(files_app_view_changes_resolve_an_offscreen_uncached_cursor_without_inventing_row_identity) {
    for (bool move_cursor : {false, true}) {
        Fixture f;
        f.loaded();
        if (move_cursor)
            for (unsigned i = 0; i < 7; ++i)
                f.key(ckv::Key::Down);
        const auto selected = f.app->target(0)->row.row;
        const auto rect = f.app->grid(0)->absolute_bounds();
        for (unsigned i = 0; i < 13; ++i)
            CK_CHECK(f.application.dispatch(ckv::MouseEvent{.action = ckv::MouseAction::Wheel,
                                                            .button = ckv::MouseButton::WheelDown,
                                                            .cell = {rect.x + 5, rect.y + 2}}));
        f.loaded();
        CK_CHECK(f.app->model(0).viewport().first > 20 && !f.app->model(0).target());
        auto view = f.app->view(0);
        view.descending = true;
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.app->view(0).descending && f.app->target(0)->row.row == selected);
        CK_CHECK(f.app->model(0).cursor().row == (move_cursor ? 92 : 99));
    }
}
CK_TEST(files_app_idle_quit_stops_services_and_restores_host_handler) {
    Fixture f;
    f.loaded();
    f.until([&] { return !f.services[0]->working() && !f.services[1]->working(); });
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    CK_CHECK(f.exits == 1 && !f.app->close_confirmation_pending() && !f.app->target(0));
    f.app.reset();
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    CK_CHECK(f.exits == 2);
}
CK_TEST(files_app_blocked_read_keeps_other_pane_live_and_close_requires_explicit_exit_choice) {
    auto gate = std::make_shared<Gate>();
    Fixture f(ThemeScheme::Classic, gate);
    gate->wait();
    f.until([&] { return !f.app->pending(1); });
    CK_CHECK(f.app->pending(0) && f.app->target(1) && !f.app->target(0));
    CK_CHECK(f.application.execute_command(f.app->switch_command()));
    CK_CHECK(f.app->active_pane() == 1);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Down, ckv::Modifier::None, {}}}));
    CK_CHECK(f.app->model(1).cursor().row == 1);
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    CK_CHECK(f.app->close_confirmation_pending() && f.exits == 0);
    CK_CHECK(!f.application.execute_command(f.app->switch_command()));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Enter, ckv::Modifier::None, {}}}));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->close_confirmation_pending() && f.exits == 0 && f.services[0]->running());
    f.app->request_close();
    CK_CHECK(f.app->close_confirmation_pending());
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Escape, ckv::Modifier::None, {}}}));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->close_confirmation_pending() && f.exits == 0);
    f.app->request_close();
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "x"}}));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.exits == 1 && f.services[0]->running()); // No join inside dispatch.
    CK_CHECK(!f.app->target(1));
    gate->release();
}
CK_TEST(files_app_cancellation_waits_for_blocked_storage_without_blocking_dispatch) {
    auto gate = std::make_shared<Gate>();
    Fixture f(ThemeScheme::Classic, gate);
    gate->wait();
    CK_CHECK(f.application.execute_command(f.app->cancel_command()));
    CK_CHECK(f.app->pending(0) && !f.app->target(0));
    f.until([&] { return !f.app->pending(1); });
    CK_CHECK(f.app->target(1));
    gate->release();
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 0 && !f.app->target(0));
    CK_CHECK(f.app->status(0).find("cancelled") != std::string::npos);
}
CK_TEST(files_app_wake_coalesces_worker_notifications_and_ignores_late_callbacks) {
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal({80, 24});
    ckv::ui::Application application(terminal, clock);
    unsigned calls = 0;
    std::function<void()> late;
    {
        FilesAppWake wake(application);
        wake.bind([&] { ++calls; });
        late = wake.callback();
        std::thread worker([&] {
            for (unsigned i = 0; i < 10000; ++i)
                late();
        });
        worker.join();
        CK_CHECK(calls == 0);
        application.step(0);
        CK_CHECK(calls == 1);
        late();
        wake.unbind();
        application.step(0);
        CK_CHECK(calls == 1);
    }
    late();
    application.step(0);
    CK_CHECK(calls == 1);
}
