// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "monitor_service.hpp"
#include "settings.hpp"
#include "cvision/widgets/application_shell.hpp"
#include "cvision/widgets/common_components.hpp"
#include <array>
#include <memory>

namespace ck::sysmon {
struct CalendarProviders {
    std::function<ckv::widgets::TimeValue()> time;
    std::function<ckv::widgets::DateValue()> date;
};
struct SettingsBinding {
    Settings value;
    std::function<std::string(const Settings&)> save;
    std::string diagnostic;
};
class SysmonApp {
public:
    SysmonApp(ckv::ui::Application& app, MonitorSource& source, CalendarProviders calendar, SettingsBinding settings = {});
    ~SysmonApp();
    SysmonApp(const SysmonApp&) = delete;
    SysmonApp& operator=(const SysmonApp&) = delete;
    ckv::widgets::Desktop& desktop() { return shell_->desktop(); }
    const std::vector<HostState>& hosts() const { return hosts_; }
    void wait_idle() { service_->wait_idle(); }
    void refresh();
    void open_calendar();
    // Public deterministic injection path used by replay/tests and live service.
    void accept(std::string endpoint, Sample sample);
private:
    void build_shell();
    void menus();
    void status();
    void paint(ckv::scene::Painter& painter, ckv::Rect area);
    void paint_host(ckv::scene::Painter& painter, ckv::Rect card, const HostState& host);
    void changed();
    void save_settings();
    std::string host_summary();
    ckv::ui::CommandId command(ckv::ui::CommandDescriptor descriptor);
    ckv::ui::Application& app_;
    CalendarProviders calendar_;
    ckv::ui::StandardRoles roles_;
    std::vector<HostState> hosts_;
    std::vector<ckv::ui::CommandId> commands_;
    std::array<ckv::ui::CommandId, 6> toggles_{};
    std::array<ckv::ui::CommandId, 2> theme_commands_{};
    SettingsBinding settings_;
    std::vector<ckv::ui::CommandId> host_commands_;
    ckv::ui::CommandId refresh_ = ckv::ui::kInvalidCommand, page_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId detail_command_ = ckv::ui::kInvalidCommand, about_ = ckv::ui::kInvalidCommand, calendar_command_ = ckv::ui::kInvalidCommand;
    std::size_t page_ = 0, detail_page_ = 0;
    int ticks_ = 0;
    ckv::ui::Application::TimerId timer_ = 0;
    ckv::widgets::ClockView* clock_ = nullptr;
    ckv::ui::View* header_ = nullptr;
    std::unique_ptr<ckv::widgets::ApplicationShell> shell_;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::unique_ptr<MonitorService> service_;
};
} // namespace ck::sysmon
