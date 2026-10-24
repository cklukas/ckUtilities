// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "scratch_directory.hpp"
#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/testing/cktest.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
namespace {
using namespace ck::vision;
using namespace ck::files;
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
    explicit Fixture(ThemeScheme theme = ThemeScheme::Classic, std::shared_ptr<Gate> blocked = {})
        : gate(std::move(blocked)) {
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
            services[i] =
                std::make_unique<FilesPaneService>(std::move(backend), FilesPaneServiceLimits{}, wake.callback());
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
    CK_CHECK(f.application.commands().execute(f.app->switch_command()));
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
CK_TEST(files_app_idle_quit_stops_services_and_restores_host_handler) {
    Fixture f;
    f.loaded();
    f.until([&] { return !f.services[0]->working() && !f.services[1]->working(); });
    CK_CHECK(f.application.commands().execute(f.application.commands().standard().quit));
    CK_CHECK(f.exits == 1 && !f.app->close_confirmation_pending() && !f.app->target(0));
    f.app.reset();
    CK_CHECK(f.application.commands().execute(f.application.commands().standard().quit));
    CK_CHECK(f.exits == 2);
}
CK_TEST(files_app_blocked_read_keeps_other_pane_live_and_close_requires_explicit_exit_choice) {
    auto gate = std::make_shared<Gate>();
    Fixture f(ThemeScheme::Classic, gate);
    gate->wait();
    f.until([&] { return !f.app->pending(1); });
    CK_CHECK(f.app->pending(0) && f.app->target(1) && !f.app->target(0));
    CK_CHECK(f.application.commands().execute(f.app->switch_command()));
    CK_CHECK(f.app->active_pane() == 1);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Down, ckv::Modifier::None, {}}}));
    CK_CHECK(f.app->model(1).cursor().row == 1);
    CK_CHECK(f.application.commands().execute(f.application.commands().standard().quit));
    CK_CHECK(f.app->close_confirmation_pending() && f.exits == 0);
    CK_CHECK(!f.application.commands().execute(f.app->switch_command()));
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
    CK_CHECK(f.application.commands().execute(f.app->cancel_command()));
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
