// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysmon_test_fixture.hpp"
#include "frame_svg.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
void capture(ck::testing::SysmonFixture& fixture, const std::filesystem::path& output,
             std::string_view golden, std::string_view name) {
    std::ifstream input(std::filesystem::path(CKTOOLS_SYSMON_GOLDENS) / (std::string(golden) + ".dump"), std::ios::binary);
    const std::string expected((std::istreambuf_iterator<char>(input)), {});
    if (!input || fixture.frame() != expected) throw std::runtime_error("sysmon screenshot differs from golden: " + std::string(golden));
    ckv::docgen::verify_decoded_cells(fixture.app, fixture.terminal);
    std::ofstream image(output / (std::string(name) + ".svg"), std::ios::binary);
    image << ckv::docgen::render_virtual_display_svg(fixture.terminal.display());
    if (!image) throw std::runtime_error("cannot write sysmon screenshot");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path output(argv[1]); std::filesystem::create_directories(output);
    ck::testing::SysmonFixture standard;
    capture(standard, output, "sysmon_120x34", "sysmon-standard");
    standard.command("theme.dark");
    capture(standard, output, "sysmon_dark_120x34", "sysmon-dark");
    ck::testing::SysmonFixture apple;
    apple.monitor->accept("", ck::testing::apple_monitor_sample());
    capture(apple, output, "sysmon_apple_120x34", "sysmon-apple");
    ck::testing::SysmonFixture stacked({80, 34});
    capture(stacked, output, "sysmon_stacked_80x34", "sysmon-stacked");
    ck::testing::SysmonFixture v3d;
    v3d.monitor->accept("", ck::testing::v3d_monitor_sample());
    auto sample = ck::testing::v3d_monitor_sample();
    sample.gpus[0].engines = {{"bin", 2000, 300}, {"render", 2000, 700}};
    v3d.monitor->accept("", sample);
    capture(v3d, output, "sysmon_v3d_120x34", "sysmon-v3d");
}
