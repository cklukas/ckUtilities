// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysmon_test_fixture.hpp"
#include "cvision/scene/svg_snapshot.hpp"
#include <filesystem>
#include <fstream>
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const auto save = [&](ck::testing::SysmonFixture& fixture, const std::string& name) {
        std::ofstream output(std::filesystem::path(argv[1]) / (name + ".dump"), std::ios::binary);
        output << fixture.frame(); if (!output) return false;
        if (argc == 3) {
            std::ofstream image(std::filesystem::path(argv[2]) / (name + ".svg"));
            image << ckv::scene::render_svg_snapshot(fixture.app.composed_surface().view());
            if (!image) return false;
        }
        return true;
    };
    for (const auto size : {ckv::Size{120, 34}, ckv::Size{80, 24}, ckv::Size{32, 12}}) {
        ck::testing::SysmonFixture fixture(size);
        if (!save(fixture, "sysmon_" + std::to_string(size.width) + "x" + std::to_string(size.height))) return 1;
    }
    ck::testing::SysmonFixture fixture;
    fixture.command("host.0"); fixture.command("host.2");
    if (!save(fixture, "sysmon_empty_120x34")) return 1;
    ck::testing::SysmonFixture dark;
    dark.command("theme.dark");
    if (!save(dark, "sysmon_dark_120x34")) return 1;
    ck::testing::SysmonFixture stacked({80, 34});
    if (!save(stacked, "sysmon_stacked_80x34")) return 1;
    ck::testing::SysmonFixture apple;
    apple.monitor->accept("", ck::testing::apple_monitor_sample());
    if (!save(apple, "sysmon_apple_120x34")) return 1;
    ck::testing::SysmonFixture v3d;
    v3d.monitor->accept("", ck::testing::v3d_monitor_sample());
    auto sample = ck::testing::v3d_monitor_sample();
    sample.gpus[0].engines = {{"bin", 2000, 300}, {"render", 2000, 700}};
    v3d.monitor->accept("", sample);
    if (!save(v3d, "sysmon_v3d_120x34")) return 1;
}
