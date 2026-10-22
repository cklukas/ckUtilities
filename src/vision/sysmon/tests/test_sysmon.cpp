// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cvision/testing/cktest.hpp"
#include "sysmon_test_fixture.hpp"
#include "cvision/widgets/window.hpp"
#include <fstream>
#include <sstream>

using namespace ck::sysmon;
CK_TEST(sysmon_cpu_interval_excludes_idle_and_rejects_reset_or_invalid_counters) {
    CK_CHECK(cpu_percent({100, 40}, {200, 90}) == 50.0);
    CK_CHECK(!cpu_percent({100, 40}, {100, 40}));
    CK_CHECK(!cpu_percent({100, 40}, {90, 50}));
    CK_CHECK(!cpu_percent({100, 40}, {120, 80}));
    CK_CHECK(!percent(200, 100)); CK_CHECK(!percent(0, 0)); CK_CHECK(percent(0, 100) == 0.0);
}
CK_TEST(sysmon_failed_samples_clear_values_and_recovery_restores_reader_choice) {
    HostState host; host.endpoint = "remote";
    host.accept(ck::testing::monitor_sample()); CK_CHECK(host.selected);
    Sample failed = ck::testing::monitor_sample(); failed.reachable = false;
    host.accept(failed); CK_CHECK(!host.selected); CK_CHECK(!host.latest.cpu_load); CK_CHECK(host.latest.gpus.empty());
    CK_CHECK(!host.cpu_history.back()); CK_CHECK(!host.gpu_history.front().load.back());
    host.accept(ck::testing::monitor_sample()); CK_CHECK(host.selected);
    host.want_selected = false; host.selected = false;
    host.accept(failed); host.accept(ck::testing::monitor_sample()); CK_CHECK(!host.selected);
}
CK_TEST(sysmon_histories_are_bounded_and_gpu_identity_survives_reordering) {
    HostState host; auto sample = ck::testing::monitor_sample();
    host.accept(sample); std::swap(sample.gpus[0], sample.gpus[1]); host.accept(sample);
    CK_CHECK(host.gpu_history[0].id == "gpu-a"); CK_CHECK(host.gpu_history[0].load.back() == 47.0);
    for (int i = 0; i < 80; ++i) host.accept(sample);
    CK_CHECK(host.cpu_history.size() == 60); CK_CHECK(host.gpu_history[0].load.size() == 60);
    CK_CHECK(sparkline({0.0, 50.0, 100.0, {}}, 4) == "·▄█·");
}
CK_TEST(sysmon_wire_round_trip_preserves_two_cards_and_models) {
    const auto sample = ck::testing::monitor_sample();
    const auto parsed = parse_sample(sample_text(sample));
    CK_CHECK(parsed); CK_CHECK(parsed->gpus.size() == 2); CK_CHECK(parsed->gpus[0].shared == sample.gpus[0].shared);
    CK_CHECK(parsed->models.size() == 1); CK_CHECK(parsed->models[0].name == sample.models[0].name);
    CK_CHECK(!parse_sample("CKSYSMON 3\nEND\n"));
    CK_CHECK(!parse_sample("CKSYSMON 2\nSYSTEM\tLinux\nRAM\t11\t10\nEND\n"));
    CK_CHECK(!parse_sample(sample_text(sample) + "unexpected\n"));
    auto invalid = sample; invalid.cpu_load = 101; CK_CHECK(!parse_sample(sample_text(invalid)));
    invalid = sample; invalid.gpus[1].id = invalid.gpus[0].id; CK_CHECK(!parse_sample(sample_text(invalid)));
}
CK_TEST(sysmon_unified_gpu_memory_keeps_its_meaning_over_ssh_and_in_the_chart) {
    auto sample = ck::testing::apple_monitor_sample();
    const auto parsed = parse_sample(sample_text(sample)); CK_CHECK(parsed);
    CK_CHECK(parsed->gpus[0].memory_kind == GpuMemoryKind::UnifiedDriver);
    ck::testing::SysmonFixture fixture;
    fixture.monitor->accept("", *parsed);
    const auto frame = fixture.frame();
    CK_CHECK(frame.find("unified RAM (driver)") != std::string::npos);
    std::ifstream input("golden/sysmon_apple_120x34.dump", std::ios::binary);
    std::ostringstream expected; expected << input.rdbuf(); CK_CHECK(!expected.str().empty()); CK_CHECK(frame == expected.str());
    sample.gpus[0].shared = 1; CK_CHECK(!parse_sample(sample_text(sample)));
}
CK_TEST(sysmon_ollama_parser_handles_nested_metadata_unicode_empty_and_invalid_responses) {
    const auto models = parse_ollama(R"({"models":[{"name":"model\u002f\ud83d\ude80","size_vram":1024,"details":{"parameters":"8B"},"expires_at":"tomorrow"}]})");
    CK_CHECK(models); CK_CHECK(models->size() == 1); CK_CHECK((*models)[0].name == "model/🚀"); CK_CHECK((*models)[0].vram == 1024);
    CK_CHECK(parse_ollama("{\"models\":[]}")->empty());
    CK_CHECK(!parse_ollama("{}")); CK_CHECK(!parse_ollama("{\"models\":[{\"name\":\"x\",\"size_vram\":-1}]}"));
    CK_CHECK(!parse_ollama("{\"models\":[{\"name\":\"\\ud800\"}]}")); CK_CHECK(!parse_ollama("{\"models\":[]}junk"));
    CK_CHECK(!parse_ollama(std::string(32769, ' ')));
}
CK_TEST(sysmon_desktop_has_no_monitor_windows_and_host_commands_follow_connectivity) {
    ck::testing::SysmonFixture fixture;
    CK_CHECK(fixture.monitor->desktop().windows().empty());
    CK_CHECK(!fixture.command("host.1")); // offline-node sorts first
    const auto enabled = fixture.app.commands().id_for("sysmon.host.2"); CK_CHECK(enabled);
    CK_CHECK(fixture.app.commands().checked(*enabled) == true);
    CK_CHECK(fixture.command("host.2")); CK_CHECK(fixture.app.commands().checked(*enabled) == false);
    const auto frame = fixture.frame(); CK_CHECK(frame.find("LOCAL") != frame.npos); CK_CHECK(frame.find("render-node") == frame.npos);
}
CK_TEST(sysmon_local_host_can_be_hidden_and_empty_selection_survives_refresh) {
    ck::testing::SysmonFixture fixture;
    const auto local = fixture.app.commands().id_for("sysmon.host.0"); CK_CHECK(local);
    CK_CHECK(fixture.app.commands().checked(*local) == true);
    CK_CHECK(fixture.command("host.0"));
    CK_CHECK(fixture.frame().find("LOCAL") == std::string::npos);
    CK_CHECK(fixture.command("host.2"));
    fixture.monitor->refresh(); fixture.monitor->wait_idle(); fixture.app.step(0);
    CK_CHECK(fixture.app.commands().checked(*local) == false);
    const auto frame = fixture.frame();
    CK_CHECK(frame.find("No hosts selected") != frame.npos);
    std::ifstream input("golden/sysmon_empty_120x34.dump", std::ios::binary);
    std::ostringstream expected; expected << input.rdbuf(); CK_CHECK(!expected.str().empty());
    CK_CHECK(frame == expected.str());
    CK_CHECK(fixture.command("next_hosts")); CK_CHECK(fixture.command("next_details"));
    CK_CHECK(fixture.command("host.0")); CK_CHECK(fixture.frame().find("LOCAL") != std::string::npos);
}
CK_TEST(sysmon_toggles_status_and_metrics_and_can_page_to_every_resource) {
    ck::testing::SysmonFixture fixture({80, 24});
    auto frame = fixture.frame(); CK_CHECK(frame.find("[x] CPU") != frame.npos);
    CK_CHECK(fixture.command("cpu")); frame = fixture.frame(); CK_CHECK(frame.find("[ ] CPU") != frame.npos);
    std::string pages;
    for (int i = 0; i < 10; ++i) { pages += fixture.frame(); fixture.command("next_details"); }
    CK_CHECK(pages.find("GPU 1") != pages.npos); CK_CHECK(pages.find("apertus") != pages.npos);
    CK_CHECK(fixture.command("next_hosts")); CK_CHECK(fixture.frame().find("render-node") != std::string::npos);
}
CK_TEST(sysmon_about_and_calendar_use_existing_dialogs_and_injected_date) {
    ck::testing::SysmonFixture fixture;
    CK_CHECK(fixture.command("calendar")); CK_CHECK(fixture.frame().find("October") != std::string::npos);
    fixture.app.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}});
    CK_CHECK(fixture.command("about")); CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.frame().find("Copyright (c) 2026 C. Klukas. All rights reserved.") != std::string::npos);
}
CK_TEST(sysmon_teardown_with_queued_posts_withdraws_handlers_and_timers) {
    ck::testing::SysmonFixture fixture;
    fixture.monitor->refresh(); fixture.monitor->wait_idle(); fixture.monitor.reset();
    fixture.clock.advance(20'000'000'000); fixture.app.step(0);
    CK_CHECK(!fixture.app.commands().id_for("sysmon.about"));
}
CK_TEST(sysmon_frames_match_reviewed_desktop_goldens_at_wide_and_small_sizes) {
    for (const auto size : {ckv::Size{120, 34}, ckv::Size{80, 24}, ckv::Size{32, 12}}) {
        ck::testing::SysmonFixture fixture(size);
        std::ifstream input("golden/sysmon_" + std::to_string(size.width) + "x" + std::to_string(size.height) + ".dump", std::ios::binary);
        std::ostringstream expected; expected << input.rdbuf(); CK_CHECK(!expected.str().empty());
        CK_CHECK(fixture.frame() == expected.str());
    }
}

CK_TEST(sysmon_resize_reflows_desktop_and_preserves_selection_and_toggles) {
    ck::testing::SysmonFixture fixture;
    fixture.command("ram");
    fixture.terminal.resize({80, 24}); fixture.app.step(0);
    CK_CHECK(fixture.frame().find("frame 80 24") != std::string::npos);
    CK_CHECK(fixture.frame().find("[ ] RAM") != std::string::npos);
    fixture.terminal.resize({150, 42}); fixture.app.step(0);
    CK_CHECK(fixture.frame().find("render-node") != std::string::npos);
    CK_CHECK(fixture.monitor->hosts()[2].selected);
}

CK_TEST(sysmon_remote_wait_does_not_delay_local_worker_and_duplicate_requests_coalesce) {
    class BlockingSource final : public MonitorSource {
    public:
        std::mutex mutex; std::condition_variable entered, release;
        bool blocked = false, proceed = false;
        int remote_calls = 0;
        std::vector<std::string> hosts() override { return {}; }
        Sample sample(const std::string& endpoint) override {
            if (!endpoint.empty()) {
                std::unique_lock lock(mutex); ++remote_calls; blocked = true; entered.notify_all();
                release.wait(lock, [this] { return proceed; });
            }
            return ck::testing::monitor_sample();
        }
    } source;
    ckv::term::HeadlessTerminal terminal({80, 24}); ckv::ManualClock clock; ckv::ui::Application app(terminal, clock);
    auto lifetime = std::make_shared<int>(0);
    bool local_delivered = false;
    MonitorService service(app, source, lifetime, [&](std::string endpoint, Sample) { if (endpoint.empty()) local_delivered = true; });
    service.request("remote");
    { std::unique_lock lock(source.mutex); source.entered.wait(lock, [&] { return source.blocked; }); }
    service.request("remote"); service.request("");
    service.wait_idle(""); app.step(0);
    CK_CHECK(local_delivered);
    { std::lock_guard lock(source.mutex); source.proceed = true; }
    source.release.notify_all(); service.wait_idle(); app.step(0);
    CK_CHECK(local_delivered); CK_CHECK(source.remote_calls == 1);
}

CK_TEST(sysmon_windows_counter_identity_preserves_physical_cards_and_hex_casing) {
    const auto first = counter_gpu_id("pid_12_luid_0x00000000_0x001C5D31_phys_0_eng_0_engtype_3D");
    const auto second = counter_gpu_id("luid_0x00000000_0x001c5d31_phys_1");
    CK_CHECK(first == "luid_0x00000000_0x001c5d31_phys_0");
    CK_CHECK(second == "luid_0x00000000_0x001c5d31_phys_1"); CK_CHECK(first != second);
    CK_CHECK(counter_gpu_id("luid_0x00000000_0x001c5d31_phys_10") != second);
    CK_CHECK(!counter_gpu_id("luid_0x00000000_0x001c5d31_phys_"));
    CK_CHECK(!counter_gpu_id("luid_0x00000000_0x001c5d31_phys_1junk"));
}

CK_TEST(sysmon_long_host_titles_preserve_card_borders_and_neighboring_hosts) {
    class LongNameSource final : public ck::testing::FixedMonitorSource {
    public:
        std::vector<std::string> hosts() override { return {std::string(90, 'a') + "界"}; }
    } source;
    ckv::term::HeadlessTerminal terminal({120, 34}); ckv::ManualClock clock;
    ckv::ui::Application app(terminal, clock);
    ck::sysmon::SysmonApp monitor(app, source, {});
    monitor.wait_idle(); app.step(0);
    CK_CHECK(app.composed_surface().at({117, 1}).grapheme() == "┐");
    CK_CHECK(ckv::golden::serialize(ckv::scene::capture(app.composed_surface(), app.current_cursor())).find("…") != std::string::npos);
}
CK_TEST(sysmon_standard_default_and_dark_theme_recolor_chrome_and_charts_without_losing_state) {
    ck::testing::SysmonFixture fixture;
    const auto before = fixture.frame();
    const auto standard = fixture.app.commands().id_for("sysmon.theme.standard"); CK_CHECK(standard);
    const auto dark = fixture.app.commands().id_for("sysmon.theme.dark"); CK_CHECK(dark);
    CK_CHECK(fixture.app.commands().checked(*standard) == true);
    const auto chart_background = fixture.app.composed_surface().at({0, 2}).style().bg;
    const auto menu_background = fixture.app.composed_surface().at({0, 0}).style().bg;
    CK_CHECK(fixture.command("theme.dark"));
    const auto dark_frame = fixture.frame();
    CK_CHECK(fixture.app.commands().checked(*dark) == true);
    CK_CHECK(fixture.app.commands().checked(*standard) == false);
    CK_CHECK(fixture.app.composed_surface().at({0, 2}).style().bg != chart_background);
    CK_CHECK(fixture.app.composed_surface().at({0, 0}).style().bg != menu_background);
    std::ifstream input("golden/sysmon_dark_120x34.dump", std::ios::binary);
    std::ostringstream expected; expected << input.rdbuf(); CK_CHECK(!expected.str().empty());
    CK_CHECK(dark_frame == expected.str());
    CK_CHECK(fixture.command("theme.standard")); CK_CHECK(fixture.frame() == before);
}
CK_TEST(sysmon_compact_chrome_and_ollama_leave_no_blank_rows_around_stacked_panels) {
    ck::testing::SysmonFixture fixture({80, 34});
    const auto frame = fixture.frame();
    CK_CHECK(fixture.app.composed_surface().at({1, 1}).grapheme() == "┌");
    CK_CHECK(fixture.app.composed_surface().at({1, 16}).grapheme() == "└");
    CK_CHECK(fixture.app.composed_surface().at({1, 17}).grapheme() == "┌");
    CK_CHECK(fixture.app.composed_surface().at({1, 32}).grapheme() == "└");
    CK_CHECK(frame.find("0–100% bars") == frame.npos);
    std::ifstream input("golden/sysmon_stacked_80x34.dump", std::ios::binary);
    std::ostringstream expected; expected << input.rdbuf(); CK_CHECK(!expected.str().empty()); CK_CHECK(frame == expected.str());
    // Header is on the menu row and never displaces a chart row.
    std::string menu;
    for (int x = 0; x < 80; ++x) menu += fixture.app.composed_surface().at({x, 0}).grapheme();
    CK_CHECK(menu.find("ck-sysmon / 2 hosts") != menu.npos); CK_CHECK(menu.find("18:40:12") != menu.npos);
    fixture.command("cpu"); fixture.command("ram"); fixture.command("gpu"); fixture.command("vram");
    fixture.frame();
    std::string status, model;
    for (int x = 0; x < 80; ++x) {
        status += fixture.app.composed_surface().at({x, 3}).grapheme();
        model += fixture.app.composed_surface().at({x, 4}).grapheme();
    }
    CK_CHECK(status.find("Ollama: running") != status.npos);
    CK_CHECK(model.find("apertus") != model.npos); CK_CHECK(model.find("GiB GPU") != model.npos);
}
CK_TEST(sysmon_settings_codec_is_strict_and_round_trips_every_option) {
    Settings settings; settings.dark = true; settings.metrics = {false, true, false, true, false, true};
    settings.hosts = {{"", false}, {"rpi4", true}, {"render-node", false}};
    CK_CHECK(parse_settings(settings_text(settings)) == settings);
    const auto valid = settings_text(Settings{});
    CK_CHECK(!parse_settings(valid + "extra\n"));
    CK_CHECK(!parse_settings("CKSYSMON_SETTINGS 2\nTHEME dark\nMETRICS 111111\nEND\n"));
    CK_CHECK(!parse_settings("CKSYSMON_SETTINGS 1\nTHEME dark\nTHEME standard\nMETRICS 111111\nEND\n"));
    CK_CHECK(!parse_settings("CKSYSMON_SETTINGS 1\nTHEME dark\nMETRICS 11111x\nEND\n"));
    CK_CHECK(!parse_settings("CKSYSMON_SETTINGS 1\nTHEME dark\nMETRICS 111111\nHOST \"rpi4\" 1\nHOST \"rpi4\" 0\nEND\n"));
    CK_CHECK(!parse_settings(std::string(65537, ' ')));
}
CK_TEST(sysmon_preferences_autosave_and_restore_on_a_new_application_instance) {
    ckv::MemoryFileSystem files;
    SettingsStore store(files, "/user/ck-sysmon.settings");
    const auto loaded = store.load(); CK_CHECK(loaded.diagnostic.empty()); CK_CHECK(!loaded.value.dark);
    {
        ck::testing::SysmonFixture fixture({120, 34}, {loaded.value, [&](const Settings& s) { return store.save(s); }, {}});
        for (const auto* option : {"host.0", "host.2", "cpu", "ram", "gpu", "vram", "history", "ollama", "theme.dark"}) {
            CK_CHECK(fixture.command(option));
            SettingsStore checkpoint(files, "/user/ck-sysmon.settings"); CK_CHECK(checkpoint.load().diagnostic.empty());
        }
    }
    SettingsStore relaunched(files, "/user/ck-sysmon.settings");
    const auto restored = relaunched.load(); CK_CHECK(restored.diagnostic.empty());
    CK_CHECK(restored.value.dark); CK_CHECK(!restored.value.hosts.at("")); CK_CHECK(!restored.value.hosts.at("render-node"));
    for (const bool visible : restored.value.metrics) CK_CHECK(!visible);
    ck::testing::SysmonFixture fixture({120, 34}, {restored.value, [&](const Settings& s) { return relaunched.save(s); }, {}});
    CK_CHECK(!fixture.monitor->hosts()[0].selected); CK_CHECK(!fixture.monitor->hosts()[2].selected);
    CK_CHECK(fixture.frame().find("No hosts selected") != std::string::npos);
    CK_CHECK(fixture.app.commands().checked(*fixture.app.commands().id_for("sysmon.theme.dark")) == true);
    CK_CHECK(fixture.command("host.0")); CK_CHECK(fixture.frame().find("All metrics hidden") != std::string::npos);
}
CK_TEST(sysmon_settings_preserve_previous_file_on_conflict_and_report_save_failures) {
    ckv::MemoryFileSystem files;
    SettingsStore first(files, "/user/ck-sysmon.settings"); first.load(); CK_CHECK(first.save({}).empty());
    SettingsStore stale(files, "/user/ck-sysmon.settings"); stale.load();
    Settings changed; changed.dark = true; CK_CHECK(first.save(changed).empty());
    CK_CHECK(!stale.save({}).empty()); CK_CHECK(parse_settings(files.read_file("/user/ck-sysmon.settings")->contents)->dark);
    files.add_file("/bad/settings", "corrupt");
    SettingsStore corrupt(files, "/bad/settings"); CK_CHECK(!corrupt.load().diagnostic.empty());
    ck::testing::SysmonFixture fixture({120, 34}, {{}, [](const Settings&) { return "Cannot save settings"; }, {}});
    fixture.command("ram"); CK_CHECK(fixture.frame().find("Cannot save settings") != std::string::npos);
}

CK_TEST(sysmon_v3d_snapshots_are_strict_and_buffer_units_are_checked) {
    const auto engines = parse_v3d_activity("queue timestamp jobs runtime\nbin 1000 2 100\nrender 1000 2 200\ncsd 1000 0 0\ntfu 1000 0 0\ncpu 1000 0 0\ncache_clean 1000 0 0\n");
    CK_CHECK(engines); CK_CHECK(engines->size() == 4);
    CK_CHECK(!parse_v3d_activity("queue timestamp jobs runtime\nbin -1 2 1\n"));
    CK_CHECK(!parse_v3d_activity("queue timestamp jobs runtime\nbin 100 2 101\n"));
    CK_CHECK(!parse_v3d_activity("queue timestamp jobs runtime\nbin 100 2 1\nbin 100 2 1\n"));
    CK_CHECK(!parse_v3d_activity("queue time jobs runtime\nbin 100 2 1\n"));
    CK_CHECK(parse_v3d_buffers("allocated bos: 199\nallocated bo size (kb): 116944\n") == 119750656ULL);
    CK_CHECK(parse_v3d_buffers("allocated bo size (kb): 0\n") == 0);
    CK_CHECK(!parse_v3d_buffers("allocated bo size (kb): -1\n"));
    CK_CHECK(!parse_v3d_buffers("allocated bo size (kb): 18014398509481984\n"));
    CK_CHECK(!parse_v3d_buffers("allocated bo size (kb): 1 extra\n"));
    CK_CHECK(!parse_v3d_buffers("allocated bo size (kb): 1\nallocated bo size (kb): 2\n"));
}
CK_TEST(sysmon_v3d_intervals_use_busiest_engine_and_break_on_gaps_or_reset) {
    auto sample = ck::testing::v3d_monitor_sample();
    auto parsed = parse_sample(sample_text(sample)); CK_CHECK(parsed);
    CK_CHECK(parsed->gpus[0].used == 119750656ULL); CK_CHECK(!parsed->gpus[0].total);
    HostState host; host.accept(*parsed); CK_CHECK(!host.latest.gpus[0].load);
    sample.gpus[0].engines = {{"render", 2000, 700}, {"bin", 2000, 300}};
    host.accept(sample); CK_CHECK(host.latest.gpus[0].load == 50.0);
    CK_CHECK(parse_sample(sample_text(host.latest))); // wire retains counters, not a derived interval
    sample.gpus[0].engines[0].runtime = 1;
    host.accept(sample); CK_CHECK(!host.latest.gpus[0].load);
    sample.gpus[0].engines = {{"render", 3000, 2000}, {"bin", 3000, 400}};
    host.accept(sample); CK_CHECK(!host.latest.gpus[0].load); // runtime delta exceeds interval
    sample.gpus[0].engines.clear(); host.accept(sample);
    sample = ck::testing::v3d_monitor_sample(); host.accept(sample); CK_CHECK(!host.latest.gpus[0].load);
    sample.gpus[0].engines.push_back(sample.gpus[0].engines[0]); CK_CHECK(!parse_sample(sample_text(sample)));
    sample = ck::testing::v3d_monitor_sample(); sample.gpus[0].total = 1024; CK_CHECK(!parse_sample(sample_text(sample)));
}
CK_TEST(sysmon_v3d_chart_labels_buffer_accounting_without_inventing_capacity) {
    ck::testing::SysmonFixture fixture;
    fixture.monitor->accept("", ck::testing::v3d_monitor_sample());
    auto sample = ck::testing::v3d_monitor_sample();
    sample.gpus[0].engines = {{"bin", 2000, 300}, {"render", 2000, 700}};
    fixture.monitor->accept("", sample);
    const auto frame = fixture.frame();
    CK_CHECK(frame.find("busiest GPU engine") != std::string::npos);
    CK_CHECK(frame.find("114.2 MiB GPU buffers (V3D)") != std::string::npos);
    std::ifstream input("golden/sysmon_v3d_120x34.dump", std::ios::binary);
    std::ostringstream expected; expected << input.rdbuf(); CK_CHECK(frame == expected.str());
}
