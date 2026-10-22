// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cvision/testing/cktest.hpp"
#include "native_monitor.hpp"
#include "settings.hpp"
#include "scratch_directory.hpp"
#include <fstream>
#if defined(_WIN32)
#include "cvision/term/windows_filesystem.hpp"
#else
#include "cvision/term/posix_filesystem.hpp"
#endif

CK_TEST(sysmon_ssh_discovery_follows_include_patterns_quotes_cycles_and_multiple_aliases) {
    ck::testing::ScratchDirectory scratch("sysmon-config");
    const auto home = scratch.path();
    std::filesystem::create_directories(home / ".ssh/conf.d");
    std::ofstream(home / ".ssh/config") << "Host local-alias second-alias *.wild !excluded\nInclude conf.d/[a-z]*.conf\nHost=\"quoted-alias\" # comment\nMatch user example\n IdentityFile key\n";
    std::ofstream(home / ".ssh/conf.d/one.conf") << "Host nested-alias second-alias\nInclude config\n";
    std::ofstream(home / "system_config") << "Host system-alias\n";
    const auto hosts = ck::sysmon::configured_hosts(home / ".ssh/config", home / "system_config", home);
    const std::vector<std::string> expected{"local-alias", "nested-alias", "quoted-alias", "second-alias", "system-alias"};
    CK_CHECK(hosts == expected);
}
CK_TEST(sysmon_ssh_discovery_missing_configuration_is_empty) {
    ck::testing::ScratchDirectory scratch("sysmon-no-config");
    CK_CHECK(ck::sysmon::configured_hosts(scratch.path()/"missing", scratch.path()/"none", scratch.path()).empty());
}
CK_TEST(sysmon_ssh_discovery_extracts_literal_match_hosts_without_interpreting_other_criteria) {
    ck::testing::ScratchDirectory scratch("sysmon-match-config");
    const auto config = scratch.path() / "config";
    std::ofstream(config) <<
        "Match user ckgit host rpi4\n"
        "Match final originalhost first,second,*.wild,!excluded host rpi4\n"
        "Match !host negated user host\n"
        "Match exec \"echo host fake\" user host host third\n"
        "Match HOST fourth localuser originalhost\n";
    const std::vector<std::string> expected{"first", "fourth", "rpi4", "second", "third"};
    CK_CHECK(ck::sysmon::configured_hosts(config, scratch.path() / "missing", scratch.path()) == expected);
}
CK_TEST(sysmon_native_settings_publish_and_reload_without_leaving_temporary_files) {
    ck::testing::ScratchDirectory scratch("sysmon-settings");
#if defined(_WIN32)
    ckv::term::WindowsFileSystem files;
#else
    ckv::term::PosixFileSystem files;
#endif
    const auto path = (scratch.path() / "preferences/ck-sysmon.settings").generic_string();
    ck::sysmon::Settings settings; settings.dark = true; settings.metrics[0] = false;
    settings.hosts = {{"", false}, {"rpi4", true}};
    ck::sysmon::SettingsStore first(files, path); CK_CHECK(first.load().diagnostic.empty());
    CK_CHECK(first.save(settings).empty());
    ck::sysmon::SettingsStore second(files, path); const auto loaded = second.load();
    CK_CHECK(loaded.diagnostic.empty()); CK_CHECK(loaded.value == settings);
    for (const auto& entry : files.list_directory(files.parent(path)))
        CK_CHECK(entry.name.find(".ckvision-tmp.") == std::string::npos);
}

#if !defined(_WIN32)
CK_TEST(sysmon_linux_drm_discovers_v3d_card_index_and_optional_counters) {
    ck::testing::ScratchDirectory scratch("sysmon-v3d");
    const auto cards = scratch.path()/"drm", debug = scratch.path()/"debug";
    std::filesystem::create_directories(cards/"card1/device");
    std::filesystem::create_directories(cards/"card1-HDMI-A-1");
    std::filesystem::create_directories(debug/"1");
    std::filesystem::create_directory_symlink("/drivers/v3d", cards/"card1/device/driver");
    std::ofstream(cards/"card1/device/gpu_stats") << "queue timestamp jobs runtime\nrender 1000 3 200\nbin 1000 3 100\n";
    std::ofstream(debug/"1/bo_stats") << "allocated bo size (kb): 116944\n";
    ck::sysmon::Sample sample;
    ck::sysmon::linux_drm_gpus(sample, cards, debug);
    CK_CHECK(sample.gpus.size() == 1); CK_CHECK(sample.gpus[0].name == "V3D card1");
    CK_CHECK(sample.gpus[0].used == 119750656ULL); CK_CHECK(!sample.gpus[0].total);
    CK_CHECK(sample.gpus[0].engines.size() == 2);
    std::filesystem::remove(debug/"1/bo_stats");
    std::ofstream(cards/"card1/device/gpu_stats") << "incompatible future format";
    ck::sysmon::Sample missing;
    ck::sysmon::linux_drm_gpus(missing, cards, debug);
    CK_CHECK(missing.gpus.size() == 1); CK_CHECK(!missing.gpus[0].used); CK_CHECK(missing.gpus[0].engines.empty());
}
#endif
