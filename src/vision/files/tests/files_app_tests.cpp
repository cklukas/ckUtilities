// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "files_location_text.hpp"
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
class ReadGateFile final : public IndexByteFile {
    std::unique_ptr<IndexByteFile> file_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<std::atomic<bool>> armed_;

  public:
    ReadGateFile(std::unique_ptr<IndexByteFile> file, std::shared_ptr<Gate> gate,
                 std::shared_ptr<std::atomic<bool>> armed)
        : file_(std::move(file)), gate_(std::move(gate)), armed_(std::move(armed)) {}
    Observation<std::uint64_t> size() override { return file_->size(); }
    Observation<std::size_t> write_at(std::uint64_t at, std::span<const std::byte> bytes) override {
        return file_->write_at(at, bytes);
    }
    Observation<std::size_t> read_at(std::uint64_t at, std::span<std::byte> bytes) override {
        if (armed_->exchange(false))
            gate_->block();
        return file_->read_at(at, bytes);
    }
};
class ReadGateFactory final : public IndexFileFactory {
    std::shared_ptr<IndexFileFactory> factory_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<std::atomic<bool>> armed_;

  public:
    ReadGateFactory(std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<Gate> gate,
                    std::shared_ptr<std::atomic<bool>> armed)
        : factory_(std::move(factory)), gate_(std::move(gate)), armed_(std::move(armed)) {}
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        auto result = factory_->create_file();
        if (!result.value())
            return result;
        return Observation<std::unique_ptr<IndexByteFile>>::available(
            std::make_unique<ReadGateFile>(std::move(result).take_value(), gate_, armed_));
    }
};
struct Fixture {
    ck::testing::ScratchDirectory scratch{"files-app", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT};
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal{ckv::Size{120, 34}};
    ckv::ui::Application application{terminal, clock};
    FilesAppWake wake{application};
    std::shared_ptr<Gate> gate;
    std::shared_ptr<Gate> directory_gate = std::make_shared<Gate>();
    std::shared_ptr<std::atomic<bool>> block_directory = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<Gate> home_gate = std::make_shared<Gate>();
    std::shared_ptr<std::atomic<bool>> block_home = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> fail_home = std::make_shared<std::atomic<bool>>(false);
    std::array<std::unique_ptr<FilesPaneService>, 2> services;
    std::array<std::unique_ptr<FilesCompletionService>, 2> completion_services;
    std::unique_ptr<FilesApp> app;
    unsigned exits = 0;
    explicit Fixture(ThemeScheme theme = ThemeScheme::Classic, std::shared_ptr<Gate> blocked = {},
                     FilesPaneServiceLimits limits = {}, ckv::Size size = {120, 34},
                     std::shared_ptr<std::atomic<bool>> fail_notification = {}, bool folders = false,
                     bool missing_initial = false, bool rejected_initial = false)
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
            if (folders) {
                const auto folder = dir / (i == 0 ? "left-item-0" : "right-item-0");
                std::filesystem::remove(folder);
                std::filesystem::create_directory(folder);
                std::filesystem::create_directory(folder / "nested");
                {
                    std::ofstream file(folder / "inside.txt");
                    file << "inside";
                }
                {
                    std::ofstream file(folder / "nested" / "leaf.txt");
                    file << "leaf";
                }
            }
            auto backend = native_files_pane_backend(address(cache));
            backend.home = [home = address(scratch.path() / "right"), hold = home_gate, arm = block_home,
                            fail = fail_home](NativePathLimits, std::stop_token) {
                if (arm->exchange(false))
                    hold->block();
                if (fail->load())
                    return Observation<NativeAbsolutePath>::unavailable(ObservationState::Denied,
                                                                        {ErrorDomain::Adapter, 42, "injected.home"});
                return Observation<NativeAbsolutePath>::available(home);
            };
            if (i == 0) {
                backend.workspace = [open = std::move(backend.workspace), hold = directory_gate,
                                     arm = block_directory]() -> Observation<std::shared_ptr<IndexFileFactory>> {
                    auto result = open();
                    if (!result.value())
                        return result;
                    return Observation<std::shared_ptr<IndexFileFactory>>::available(
                        std::make_shared<ReadGateFactory>(*result.value(), hold, arm));
                };
            }
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
            const auto completion_cache = scratch.path() / (i == 0 ? "completion-cache-left" : "completion-cache-right");
            std::filesystem::create_directory(completion_cache);
#ifndef _WIN32
            std::filesystem::permissions(completion_cache, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace);
#endif
            completion_services[i] =
                std::make_unique<FilesCompletionService>(native_files_pane_backend(address(completion_cache)),
                                                         FilesCompletionServiceLimits{}, wake.callback());
        }
        app = std::make_unique<FilesApp>(
            application, std::array<FilesPaneService *, 2>{services[0].get(), services[1].get()}, wake,
            std::array<NativeAbsolutePath, 2>{
                address(scratch.path() / (rejected_initial ? std::string(1536, 'x')
                                                           : std::string(missing_initial ? "missing" : "left"))),
                address(scratch.path() / "right")},
            [this] { ++exits; }, theme,
            std::array<FilesCompletionService *, 2>{completion_services[0].get(), completion_services[1].get()});
    }
    ~Fixture() {
        app.reset();
        if (gate)
            gate->release();
        directory_gate->release();
        home_gate->release();
        for (auto &service : services)
            service->join();
        for (auto &service : completion_services)
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
    bool location_key() { return application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Ctrl, "l"}}); }
    bool alt(std::string text) {
        return application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, std::move(text)}});
    }
    ckv::widgets::InputLine *editor_input() {
        auto *input = dynamic_cast<ckv::widgets::InputLine *>(application.focused());
        if (!input)
            throw std::runtime_error("location editor did not focus its real input");
        return input;
    }
    std::string path_text(const std::filesystem::path &path) {
        const auto value = address(path);
        if (value.posix_bytes())
            return *value.posix_bytes();
        std::string text;
        for (auto unit : *value.windows_units()) {
            if (unit > 0x7f)
                throw std::runtime_error("fixture root is not ASCII");
            text.push_back(static_cast<char>(unit));
        }
        return text;
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

CK_TEST(files_app_enter_folder_preserves_view_policy_clears_filter_and_commits_only_after_acknowledgment) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme, {}, {}, {120, 34}, {}, true);
        f.loaded();
        auto view = f.app->view(0);
        view.descending = view.folders_first = view.show_hidden = view.ascii_case_sensitive = true;
        view.display_substring = "left-item-0";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
        f.loaded();
        const auto previous = f.app->location(0), other = f.app->location(1);
        const auto other_target = f.app->target(1)->row.row;
        CK_CHECK(f.key(ckv::Key::Enter));
        CK_CHECK(f.app->pending(0) && !f.app->target(0) && f.app->location(0) == previous);
        f.loaded();
        CK_CHECK(f.app->location(0)->root_address() == previous->root_address());
        CK_CHECK(f.app->location(0)->components().size() == 1 &&
                 f.app->location(0)->components()[0].display_name() == "left-item-0");
        CK_CHECK(f.app->model(0).row_count() == 2 && f.app->view(0).display_substring.empty());
        CK_CHECK(f.app->view(0).descending && f.app->view(0).folders_first && f.app->view(0).show_hidden &&
                 f.app->view(0).ascii_case_sensitive);
        CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.mark_range"));
        CK_CHECK(f.app->location(1) == other && f.app->target(1)->row.row == other_target);
        const auto text = f.painted();
        CK_CHECK(text.find("nested") != text.npos && text.find("inside.txt") != text.npos);
        CK_CHECK(f.app->status(0).find("Range start set") == std::string::npos);
    }
}
CK_TEST(files_app_folder_double_click_routes_to_the_clicked_pane_and_keeps_the_other_live) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    const auto left = f.app->location(0);
    const auto rect = f.app->grid(1)->cell_rect({0, 1});
    if (!rect)
        throw std::runtime_error("missing folder cell");
    const auto grid = f.app->grid(1)->absolute_bounds();
    const ckv::Point at{grid.x + rect->x + rect->width / 2, grid.y + rect->y};
    for (int click : {1, 2}) {
        CK_CHECK(f.application.dispatch(ckv::MouseEvent{
            .action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = at, .click_count = click}));
        (void)f.application.dispatch(ckv::MouseEvent{
            .action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = at, .click_count = click});
    }
    CK_CHECK(f.app->active_pane() == 1 && f.app->pending(1));
    f.loaded();
    CK_CHECK(f.app->location(1)->components().size() == 1 &&
             f.app->location(1)->components()[0].display_name() == "right-item-0");
    CK_CHECK(f.app->location(0) == left && f.app->model(0).row_count() == 100 && f.app->target(0));
}
CK_TEST(files_app_folder_keyboard_activation_uses_live_keymap_and_leaves_F2_to_command_routing) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    f.application.commands().unbind_key({ckv::Key::Enter, ckv::Modifier::None, {}});
    CK_CHECK(!f.key(ckv::Key::Enter) && !f.key(ckv::Key::F2) && !f.app->pending(0));
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.enter_directory"));
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
}
CK_TEST(files_app_folder_menu_and_scope_do_not_enter_files_or_run_from_dialogs_and_unrelated_inputs) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.key(ckv::Key::Down));
    CK_CHECK(!f.command("ck.files.enter_directory") && !f.app->enter_directory(0));
    CK_CHECK(f.key(ckv::Key::Home));
    auto input = std::make_unique<ckv::widgets::InputLine>();
    input->set_bounds({0, 0, 20, 1});
    auto *outside = f.application.root().add_child(std::move(input));
    f.application.set_focus(outside);
    CK_CHECK(!f.command("ck.files.enter_directory"));
    f.application.root().remove_child(outside);
    f.application.set_focus(f.app->grid(0));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(!f.command("ck.files.enter_directory") && !f.app->enter_directory(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "e"}}));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
}
CK_TEST(files_app_failed_folder_entry_retains_location_rows_marks_range_and_cursor) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto location = f.app->location(0);
    const auto row = f.app->target(0)->row.row;
    std::filesystem::remove_all(f.scratch.path() / "left" / "left-item-0");
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(0) == location && f.app->model(0).row_count() == 100 && f.app->target(0)->row.row == row);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
    CK_CHECK(f.command("ck.files.mark_range"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 1);
}
CK_TEST(files_app_blocked_folder_entry_freezes_that_pane_and_cancels_without_retargeting_other_pane) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    const auto before = f.app->location(0);
    const auto cursor = f.app->model(0).cursor().row;
    *f.block_directory = true;
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.directory_gate->wait();
    CK_CHECK(f.app->pending(0) && !f.app->target(0) && f.app->status(0) == "Opening folder...");
    f.key(ckv::Key::Down);
    CK_CHECK(f.app->model(0).cursor().row == cursor);
    f.app->switch_pane();
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(1).cursor().row == 1 && f.app->target(1));
    f.app->switch_pane();
    CK_CHECK(f.command("ck.files.cancel_read"));
    f.directory_gate->release();
    f.loaded();
    CK_CHECK(f.app->location(0) == before && f.app->model(0).cursor().row == cursor && f.app->target(0));
    CK_CHECK(f.app->status(0).find("Read cancelled") != std::string::npos);
}
CK_TEST(files_app_folder_cancellation_after_commit_reports_the_opened_location) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.services[0]->working()) {
        if (std::chrono::steady_clock::now() >= until)
            throw std::runtime_error("folder worker did not finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    f.app->cancel_read();
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
    CK_CHECK(f.app->status(0).find("Location opened before cancellation completed") != std::string::npos);
}
CK_TEST(files_app_explicit_navigation_supersedes_blocked_folder_entry) {
    Fixture f(ThemeScheme::Classic, {}, {}, {80, 24}, {}, true);
    f.loaded();
    *f.block_directory = true;
    CK_CHECK(f.app->enter_directory(0));
    f.directory_gate->wait();
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.directory_gate->release();
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
             f.app->location(0)->components().empty());
    CK_CHECK(f.app->model(0).row_count() == 3 && f.app->narrow());
    CK_CHECK(f.painted().find("Other: Right") != std::string::npos);
}
CK_TEST(files_app_folder_notification_fault_blocks_only_that_pane_without_claiming_navigation_success) {
    auto fault = std::make_shared<std::atomic<bool>>(false);
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, fault, true);
    f.loaded();
    const auto previous = f.app->location(0);
    *fault = true;
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(!f.app->target(0) && !f.app->model(0).current() && f.app->location(0) == previous);
    CK_CHECK(!f.command("ck.files.enter_directory"));
    f.app->switch_pane();
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(1)->components().size() == 1);
}

CK_TEST(files_app_folder_entry_preserves_native_names_without_parsing_display_or_flattening_windows_paths) {
    Fixture f;
    f.loaded();
    const auto root = f.scratch.path() / "left";
#ifdef _WIN32
    auto preferred = root;
    preferred.make_preferred();
    auto native = preferred.native();
    if (!native.starts_with(L"\\\\?\\"))
        native = L"\\\\?\\" + native;
    const auto folder = std::filesystem::path(native + L"\\00-literal. ");
    const auto expected = *NativeName::from_windows(u"00-literal. ");
#else
    const auto folder = root / "00-escaped\\name\n";
    const auto expected = *NativeName::from_posix(folder.filename().native());
#endif
    std::filesystem::create_directory(folder);
    {
        std::ofstream file(folder / "leaf.txt");
        file << "leaf";
        if (!file)
            throw std::runtime_error("literal folder fixture write");
    }
    CK_CHECK(f.app->navigate(0, address(root)));
    f.loaded();
    CK_CHECK(f.app->model(0).cursor_kind() == ObjectKind::Directory);
    CK_CHECK(f.painted().find(expected.display_name()) != std::string::npos);
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(root) && f.app->location(0)->components().size() == 1);
    CK_CHECK(f.app->location(0)->components()[0] == expected && f.app->model(0).row_count() == 1);
    CK_CHECK(f.painted().find("leaf.txt") != std::string::npos);
    std::filesystem::create_directory(folder / "nested");
    CK_CHECK(f.app->navigate_parent(0));
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 2);
    CK_CHECK(f.app->navigate_parent(0));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1 && f.app->location(0)->components()[0] == expected);
    CK_CHECK(f.app->model(0).row_count() == 2);
    const auto before_editor = f.app->location(0);
    CK_CHECK(f.location_key());
    const auto exact_text = format_files_exact_location(*before_editor);
    CK_CHECK(exact_text.value() && f.editor_input()->text() == *exact_text.value());
    CK_CHECK(f.alt("p")); // Refuse a lossy ordinary-path projection.
    CK_CHECK(f.editor_input()->text() == *exact_text.value());
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before_editor && !f.app->pending(0));
    std::filesystem::remove_all(folder); // Exact extended spelling on Windows.
}

CK_TEST(files_app_home_and_end_address_file_records_and_wait_for_uncached_row_identity) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.key(ckv::Key::End));
    CK_CHECK(f.app->model(0).cursor().row == 99 && !f.app->target(0));
    CK_CHECK(!f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->model(0).cursor().row == 99 && f.app->target(0));
    CK_CHECK(f.key(ckv::Key::Home));
    CK_CHECK(f.app->model(0).cursor().row == 0 && !f.app->target(0));
    f.loaded();
    CK_CHECK(f.app->target(0) && f.app->model(0).cursor_kind() == ObjectKind::Directory);
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
}

CK_TEST(files_app_parent_replays_exact_ancestors_and_preserves_view_policy_and_other_pane) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme, {}, {}, {120, 34}, {}, true);
        f.loaded();
        const auto start = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.app->enter_directory(0));
        f.loaded();
        CK_CHECK(f.app->enter_directory(0));
        f.loaded();
        CK_CHECK(f.app->location(0)->components().size() == 2);
        auto view = f.app->view(0);
        view.show_hidden = view.ascii_case_sensitive = true;
        view.display_substring = "leaf";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        const auto before = f.app->location(0);
        CK_CHECK(f.key(ckv::Key::Backspace) && f.app->pending(0) && f.app->location(0) == before);
        f.loaded();
        CK_CHECK(f.app->location(0)->components().size() == 1 && f.app->view(0).display_substring.empty());
        CK_CHECK(f.app->view(0).show_hidden && f.app->view(0).ascii_case_sensitive);
        CK_CHECK(f.command("ck.files.parent"));
        f.loaded();
        CK_CHECK(*f.app->location(0) == *start && f.app->location(1) == other);
        CK_CHECK(f.app->model(0).row_count() == 100);
    }
}
CK_TEST(files_app_parent_failure_retains_exact_route_rows_and_range_without_affecting_other_pane) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto previous = f.app->location(0), other = f.app->location(1);
    std::filesystem::remove_all(f.scratch.path() / "left" / "left-item-0");
    CK_CHECK(f.app->navigate_parent(0));
    f.loaded();
    CK_CHECK(f.app->location(0) == previous && f.app->location(1) == other && f.app->model(0).row_count() == 1);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
}
CK_TEST(files_app_root_menu_obeys_namespace_boundary_and_parent_is_disabled_at_root) {
    Fixture f(ThemeScheme::HighContrast, {}, {}, {80, 24});
    f.loaded();
    const auto expected = f.app->location(0)->namespace_root();
    const auto other = f.app->location(1);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "r"}}));
    f.loaded();
    CK_CHECK(*f.app->location(0) == *expected.value() && f.app->location(1) == other);
    CK_CHECK(!f.command("ck.files.root") && !f.command("ck.files.parent"));
    CK_CHECK(!f.app->navigate_parent(0) && !f.app->navigate_root(0) && !f.app->navigate_parent(2));
}
CK_TEST(files_app_parent_keyboard_remapping_and_modal_scope_use_suite_command_guards) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    f.application.commands().unbind_key({ckv::Key::Backspace, ckv::Modifier::None, {}});
    CK_CHECK(!f.key(ckv::Key::Backspace) && !f.app->pending(0));
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.parent"));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(!f.command("ck.files.parent") && !f.app->navigate_parent(0) && !f.app->navigate_root(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().empty());
}

CK_TEST(files_app_home_menu_discovers_asynchronously_preserves_policy_and_keeps_other_pane_independent) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        auto view = f.app->view(0);
        view.show_hidden = view.ascii_case_sensitive = true;
        view.display_substring = "left";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
        f.loaded();
        const auto before = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
        CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "h"}}));
        CK_CHECK(f.app->pending(0) && f.app->location(0) == before && f.app->selection(0).marks->total == 1);
        f.loaded();
        CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
                 f.app->location(1) == other);
        CK_CHECK(f.app->view(0).display_substring.empty() && f.app->view(0).show_hidden &&
                 f.app->view(0).ascii_case_sensitive);
        CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.mark_range") &&
                 f.app->model(0).row_count() == 3);
    }
}
CK_TEST(files_app_blocked_home_cancels_or_is_superseded_and_never_changes_the_other_pane) {
    for (bool supersede : {false, true}) {
        Fixture f(ThemeScheme::Classic, {}, {}, {80, 24});
        f.loaded();
        const auto before = f.app->location(0), other = f.app->location(1);
        *f.block_home = true;
        CK_CHECK(f.command("ck.files.home"));
        f.home_gate->wait();
        CK_CHECK(f.app->status(0) == "Opening home location..." && f.app->location(0) == before);
        CK_CHECK(!f.command("ck.files.home") && !f.app->target(0));
        f.app->switch_pane();
        CK_CHECK(f.key(ckv::Key::Down));
        f.app->switch_pane();
        if (supersede)
            CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
        else
            CK_CHECK(f.command("ck.files.cancel_read"));
        f.home_gate->release();
        f.loaded();
        CK_CHECK(f.app->location(1) == other && f.app->model(1).cursor().row == 1);
        if (supersede)
            CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
        else
            CK_CHECK(f.app->location(0) == before && f.app->status(0).find("Read cancelled") != std::string::npos);
    }
}
CK_TEST(files_app_home_discovery_failure_retains_location_filter_marks_and_range_without_fallback) {
    Fixture f;
    f.loaded();
    auto view = f.app->view(0);
    view.display_substring = "left";
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto before = f.app->location(0);
    *f.fail_home = true;
    CK_CHECK(f.app->navigate_home(0));
    f.loaded();
    CK_CHECK(f.app->location(0) == before && f.app->view(0).display_substring == "left" &&
             f.app->model(0).row_count() == 100);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
}
CK_TEST(files_app_home_obeys_modal_scope_and_configurable_keymap) {
    Fixture f;
    f.loaded();
    CK_CHECK(!f.app->navigate_home(2));
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.home"));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(!f.command("ck.files.home") && !f.app->navigate_home(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl));
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
}
CK_TEST(files_app_location_editor_ctrl_l_changes_only_its_pane_and_preserves_view_policy_in_all_themes) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        auto view = f.app->view(0);
        view.show_hidden = view.descending = true;
        view.display_substring = "left";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
        f.loaded();
        const auto before = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.location_key() && f.app->location_editor_pending());
        CK_CHECK(!f.command("ck.files.home") && !f.command("ck.files.toggle_mark"));
        f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
        CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
        CK_CHECK(f.key(ckv::Key::Enter));
        f.until([&] { return !f.app->location_editor_pending(); });
        f.loaded();
        CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
                 f.app->location(1) == other);
        CK_CHECK(f.app->view(0).descending && f.app->view(0).show_hidden && f.app->view(0).display_substring.empty());
        CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.mark_range"));
        CK_CHECK(f.application.focused() == f.app->grid(0));
    }
}
CK_TEST(files_app_location_editor_uses_worker_completion_and_right_accepts_the_exact_directory_proposal) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.location_key());
    auto *field = f.editor_input();
    const auto prefix = f.path_text(f.scratch.path() / "left") + "/left-item-";
    field->set_text(prefix);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!field->completion_candidate() && std::chrono::steady_clock::now() < deadline) {
        f.application.step(f.clock.now_nanos());
        f.clock.advance(1'000'000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(field->completion_candidate() && field->has_completion_preview());
    if (field->completion_candidate()) {
        CK_CHECK(field->completion_candidate()->text == prefix + "0/");
        CK_CHECK(f.key(ckv::Key::Right));
        CK_CHECK(field->text() == prefix + "0/");
    }
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0)->components().empty());
}
CK_TEST(files_app_location_editor_unchanged_accept_and_format_switches_preserve_exact_route_marks_and_range) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto before = f.app->location(0);
    const auto target = *f.app->target(0);
    CK_CHECK(before->components().size() == 1 && f.location_key());
    CK_CHECK(f.alt("e"));
    const auto exact = format_files_exact_location(*before);
    CK_CHECK(exact.value() && f.editor_input()->text() == *exact.value());
    CK_CHECK(f.alt("p"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
    CK_CHECK(f.app->target(0)->navigation == target.navigation && f.app->target(0)->row.row == target.row.row);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
}
CK_TEST(files_app_location_editor_vetoes_invalid_text_and_actual_service_limits_without_native_navigation) {
    FilesPaneServiceLimits limits;
    limits.location_components = 1;
    Fixture f(ThemeScheme::Classic, {}, limits);
    f.loaded();
    const auto before = f.app->location(0);
    CK_CHECK(f.location_key());
    f.editor_input()->set_text("relative");
    CK_CHECK(f.key(ckv::Key::Enter) && f.app->location_editor_pending() && !f.editor_input()->valid());
    CK_CHECK(f.painted().find("Enter a valid absolute") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
    CK_CHECK(f.location_key() && f.alt("e"));
    auto route = *before;
    for (auto name : {"first", "second"}) {
#ifdef _WIN32
        const auto native =
            *NativeName::from_windows(std::u16string(name, name + std::char_traits<char>::length(name)));
#else
        const auto native = *NativeName::from_posix(name);
#endif
        const auto next = route.descend(native, {}, 4);
        if (!next.value())
            throw std::runtime_error("limit fixture route");
        route = *next.value();
    }
    const auto exact = format_files_exact_location(route);
    CK_CHECK(exact.value());
    f.editor_input()->set_text(*exact.value());
    CK_CHECK(f.key(ckv::Key::Enter) && f.app->location_editor_pending());
    CK_CHECK(f.painted().find("exceeds a configured") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
}
CK_TEST(files_app_location_editor_recovers_an_initial_native_failure_without_a_completed_location) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, false, true);
    f.loaded();
    CK_CHECK(!f.app->location(0) && !f.app->model(0).current());
    CK_CHECK(f.location_key());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
             f.app->model(0).row_count() == 3);
}
CK_TEST(files_app_location_editor_empty_recovery_never_guesses_a_target_after_initial_submission_rejection) {
    FilesPaneServiceLimits limits;
    limits.path_bytes = 1024;
    Fixture f(ThemeScheme::Classic, {}, limits, {120, 34}, {}, false, false, true);
    f.loaded();
    const auto other = f.app->location(1);
    CK_CHECK(!f.app->location(0) && f.location_key() && f.editor_input()->text().empty());
    CK_CHECK(f.key(ckv::Key::Enter) && f.app->location_editor_pending());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") && f.app->model(0).current());
    CK_CHECK(f.app->location(1) == other);
}
CK_TEST(files_app_location_editor_does_not_retarget_a_pane_superseded_while_the_form_is_open) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.location_key());
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.loaded();
    const auto replacement = f.app->location(0);
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "left"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == replacement && !f.app->pending(0));
    CK_CHECK(f.app->status(0).find("Location changed while its editor was open") != std::string::npos);
}
CK_TEST(files_app_location_editor_quit_retirement_and_external_detach_never_submit_unaccepted_text) {
    for (int action : {0, 1, 2, 3}) {
        Fixture f;
        f.loaded();
        const auto before = f.app->location(0);
        CK_CHECK(f.location_key());
        f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
        if (action == 0) {
            f.app->request_close();
            CK_CHECK(f.exits == 0);
            f.until([&] { return f.exits == 1; });
            CK_CHECK(f.app->location(0) == before);
        } else if (action == 1) {
            f.app.reset();
            f.application.step(f.clock.now_nanos());
            CK_CHECK(f.exits == 0);
        } else {
            ckv::widgets::Window *dialog = nullptr;
            for (auto *view = f.application.focused(); view; view = view->parent())
                if (auto *candidate = dynamic_cast<ckv::widgets::Window *>(view)) {
                    dialog = candidate;
                    break;
                }
            if (!dialog || !dialog->parent())
                throw std::runtime_error("missing attached location window");
            if (action == 3)
                CK_CHECK(f.key(ckv::Key::Enter));
            auto detached = dialog->parent()->remove_child(dialog);
            (void)detached;
            if (action == 3) {
                f.app.reset();
                f.application.step(f.clock.now_nanos());
                CK_CHECK(f.exits == 0);
            } else {
                f.until([&] { return !f.app->location_editor_pending(); });
                CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
            }
        }
    }
}
CK_TEST(files_app_location_editor_scrolls_after_narrow_resize_and_accepts_through_the_real_go_button) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.location_key());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    f.terminal.resize({40, 12});
    f.application.step(f.clock.now_nanos());
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), buttons);
    ckv::widgets::Button *go = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Go")
            go = button;
    if (!go)
        throw std::runtime_error("missing real Go button");
    const auto rect = go->absolute_bounds();
    CK_CHECK(rect.width > 0 && rect.height > 0);
    const ckv::Point at{rect.x + rect.width / 2, rect.y};
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = at}));
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = at}));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
}
CK_TEST(files_app_location_command_obeys_runtime_remapping_and_unrelated_focus_scope) {
    Fixture f;
    f.loaded();
    auto other = std::make_unique<ckv::widgets::InputLine>();
    other->set_bounds({0, 0, 20, 1});
    auto *outside = f.application.root().add_child(std::move(other));
    f.application.set_focus(outside);
    CK_CHECK(!f.command("ck.files.location"));
    f.application.root().remove_child(outside);
    f.application.set_focus(f.app->grid(0));
    f.application.commands().unbind_key({ckv::Key::Char, ckv::Modifier::Ctrl, "l"});
    CK_CHECK(!f.location_key() && !f.app->location_editor_pending());
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.location"));
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl) && f.app->location_editor_pending());
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
}
CK_TEST(files_app_location_editor_can_supersede_a_blocked_home_request_without_waiting_in_dispatch) {
    Fixture f;
    f.loaded();
    const auto before = f.app->location(0);
    *f.block_home = true;
    CK_CHECK(f.app->navigate_home(0));
    f.home_gate->wait();
    CK_CHECK(f.app->pending(0) && f.location_key() && f.app->location_editor_pending());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->pending(0) && f.app->location(0) == before);
    f.home_gate->release();
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
}
CK_TEST(files_app_history_and_bookmarks_change_only_after_worker_acknowledgment) {
    Fixture f;
    f.loaded();
    const auto left = f.app->location(0);
    const auto right = address(f.scratch.path() / "right");
    CK_CHECK(f.app->history_size(0) == 1 && f.app->history_position(0) == 0);
    CK_CHECK(f.app->navigate(0, right));
    CK_CHECK(f.app->history_size(0) == 1);
    f.loaded();
    CK_CHECK(f.app->history_size(0) == 2 && f.app->history_position(0) == 1 &&
             f.app->location(0)->root_address() == right);
    CK_CHECK(f.app->add_bookmark(0) && f.app->bookmark_count(0) == 1);
    CK_CHECK(!f.app->add_bookmark(0) && f.app->bookmark_count(0) == 1);
    CK_CHECK(f.app->navigate_history_back(0));
    CK_CHECK(f.app->pending(0) && f.app->location(0) && *f.app->location(0) != *left);
    f.loaded();
    CK_CHECK(f.app->history_position(0) == 0 && f.app->location(0) && *f.app->location(0) == *left);
    CK_CHECK(f.app->navigate_history_forward(0));
    f.loaded();
    CK_CHECK(f.app->history_position(0) == 1 && f.app->location(0)->root_address() == right);
    CK_CHECK(f.app->navigate_bookmark(0, 0));
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == right);
    CK_CHECK(f.app->remove_bookmark(0) && f.app->bookmark_count(0) == 0);
}
