// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysmon_app.hpp"
#include "host_about.hpp"
#include "cvision/scene/painter.hpp"
#include "cvision/core/text.hpp"
#include "cvision/widgets/message_box.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ck::sysmon {
namespace {
struct ChartPalette {
    ckv::Style background, frame, heading, quiet, green, amber, red;
};
ChartPalette chart_palette(bool dark) {
    const auto bg = dark ? ckv::Color::rgb(15, 27, 43) : ckv::Color::rgb(240, 244, 248);
    if (dark) return {
        {ckv::Color::rgb(183, 199, 218), bg}, {ckv::Color::rgb(65, 112, 147), bg},
        {ckv::Color::rgb(112, 220, 229), bg, ckv::Attr::Bold}, {ckv::Color::rgb(122, 143, 165), bg},
        {ckv::Color::rgb(118, 221, 151), bg}, {ckv::Color::rgb(246, 192, 105), bg}, {ckv::Color::rgb(242, 120, 123), bg}};
    return {
        {ckv::Color::rgb(35, 51, 68), bg}, {ckv::Color::rgb(128, 148, 166), bg},
        {ckv::Color::rgb(17, 79, 104), bg, ckv::Attr::Bold}, {ckv::Color::rgb(74, 93, 111), bg},
        {ckv::Color::rgb(20, 105, 53), bg}, {ckv::Color::rgb(145, 85, 5), bg}, {ckv::Color::rgb(172, 42, 45), bg}};
}
std::string decimal(double value) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(1) << value; return out.str();
}
struct Row {
    std::string label, detail;
    std::optional<double> value;
    const History* history = nullptr;
    bool metric = true;
    bool compact = false;
};
struct HostGrid {
    int columns, rows;
    std::size_t per_page, pages;
};
HostGrid host_grid(ckv::Rect area, std::size_t hosts) {
    const int columns = std::max(1, std::min(static_cast<int>(hosts), area.width / 52));
    const int rows = std::max(1, std::min(area.height / 15, (static_cast<int>(hosts) + columns - 1) / columns));
    const auto per_page = static_cast<std::size_t>(columns * rows);
    return {columns, rows, per_page, std::max(std::size_t{1}, (hosts + per_page - 1) / per_page)};
}
// One menu-bar accessory: a passive summary and the existing interactive clock.
// The clock keeps its keyboard menu walk and calendar anchor; menus always get
// their required width first, then the summary shrinks ahead of the clock.
class MonitorHeader final : public ckv::ui::View, public ckv::widgets::MenuBarAccessory {
public:
    MonitorHeader(std::unique_ptr<ckv::widgets::ClockView> clock, std::function<std::string()> summary)
        : summary_(std::move(summary)), clock_(clock.get()) { add_child(std::move(clock)); }
    ckv::ui::SizeHint horizontal_size_hint() const override {
        const int clock_width = clock_->horizontal_size_hint().preferred;
        return {clock_width, clock_width + 48, clock_width + 48};
    }
    void on_resized() override {
        const int width = std::min(bounds().width, clock_->horizontal_size_hint().preferred);
        clock_->set_bounds({bounds().width - width, 0, width, 1});
    }
    void on_child_size_hint_changed(ckv::ui::View&) override { on_resized(); size_hint_changed(); }
    void set_menu_highlighted(bool highlighted) override { clock_->set_menu_highlighted(highlighted); }
    void activate_from_menu_bar() override { clock_->activate_from_menu_bar(); }
    void draw(ckv::scene::Painter& painter) override {
        const auto role = context().roles->find("ckv.menu.bar.normal");
        const auto style = context().theme->resolve(role);
        painter.fill({0, 0, bounds().width, 1}, ckv::Cell::from_grapheme(" ", style));
        const int width = std::max(0, clock_->bounds().x - 2);
        if (width >= 8) {
            const auto title = ckv::text::elide_to_width(summary_(), width);
            painter.draw_text({width - ckv::text::text_width(title), 0}, title, style);
        }
    }
private:
    std::function<std::string()> summary_;
    ckv::widgets::ClockView* clock_ = nullptr;
};
}
SysmonApp::SysmonApp(ckv::ui::Application& app, MonitorSource& source, CalendarProviders calendar, SettingsBinding settings)
    : app_(app), calendar_(std::move(calendar)), roles_(ckv::ui::intern_standard_roles(app.roles())), settings_(std::move(settings)) {
    hosts_.push_back({});
    auto aliases = source.hosts();
    std::sort(aliases.begin(), aliases.end()); aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
    for (const auto& alias : aliases) if (!alias.empty()) { HostState host; host.endpoint = alias; hosts_.push_back(std::move(host)); }
    for (auto& host : hosts_) {
        if (const auto found = settings_.value.hosts.find(host.endpoint); found != settings_.value.hosts.end()) host.want_selected = found->second;
        if (host.endpoint.empty()) host.selected = host.want_selected;
    }
    build_shell();
    service_ = std::make_unique<MonitorService>(app_, source, lifetime_, [this](std::string host, Sample sample) { accept(std::move(host), std::move(sample)); });
    refresh();
    timer_ = app_.start_timer(1'000'000'000, true, [this] {
        ++ticks_;
        for (const auto& host : hosts_) if (host.selected || ticks_ % 15 == 0) service_->request(host.endpoint);
    });
}
SysmonApp::~SysmonApp() {
    lifetime_.reset();
    app_.cancel_timer(timer_);
    service_.reset();
    app_.set_help_provider({});
    for (const auto id : commands_) app_.commands().withdraw(id);
    app_.commands().set_handler(app_.commands().standard().quit, {});
    shell_->detach_desktop();
}
ckv::ui::CommandId SysmonApp::command(ckv::ui::CommandDescriptor descriptor) {
    descriptor.key = "sysmon." + descriptor.key;
    descriptor.category = "System monitor";
    const auto id = app_.commands().declare(std::move(descriptor));
    commands_.push_back(id); return id;
}
void SysmonApp::build_shell() {
    constexpr const char* keys[]{"cpu", "ram", "gpu", "vram", "history", "ollama"};
    constexpr const char* titles[]{"CPU", "RAM", "GPU", "GPU memory", "History", "Ollama"};
    for (std::size_t i = 0; i < toggles_.size(); ++i) {
        toggles_[i] = command({.key = keys[i], .title = titles[i], .chord = "F" + std::to_string(i + 2), .handler = [this, i] { settings_.value.metrics[i] = !settings_.value.metrics[i]; detail_page_ = 0; save_settings(); changed(); }});
        app_.commands().set_checked_predicate(toggles_[i], [this, i] { return settings_.value.metrics[i]; });
    }
    constexpr const char* theme_keys[]{"theme.standard", "theme.dark"};
    constexpr const char* theme_titles[]{"&Standard (bright)", "&Dark"};
    constexpr auto factories = std::array{ckv::ui::make_classic_theme, ckv::ui::make_dark_theme};
    for (std::size_t i = 0; i < theme_commands_.size(); ++i) {
        const auto factory = factories[i];
        theme_commands_[i] = command({.key = theme_keys[i], .title = theme_titles[i], .handler = [this, i, factory] {
            settings_.value.dark = i == 1;
            app_.set_theme(factory(app_.roles(), roles_));
            save_settings(); changed();
        }});
        app_.commands().set_checked_predicate(theme_commands_[i], [this, i] { return settings_.value.dark == (i == 1); });
    }
    refresh_ = command({.key = "refresh", .title = "&Refresh hosts", .chord = "Ctrl+R", .handler = [this] { refresh(); }});
    page_command_ = command({.key = "next_hosts", .title = "Next &hosts", .chord = "F8", .handler = [this] { ++page_; changed(); }});
    detail_command_ = command({.key = "next_details", .title = "Next &details", .chord = "F9", .handler = [this] { ++detail_page_; changed(); }});
    calendar_command_ = command({.key = "calendar", .title = "&Calendar", .chord = "Ctrl+D", .handler = [this] { open_calendar(); }});
    about_ = command({.key = "about", .title = "&About ck-sysmon...", .chord = "F1", .handler = [this] {
        auto box = ckv::widgets::present_modal_message_box(app_, desktop(), roles_,
            {ckv::widgets::MessageBoxKind::Info, "About ck-sysmon", ck::host::about_text(
                "ck-sysmon — desktop system monitor\n\nNative CPU, memory and GPU metrics; passwordless SSH hosts; loaded Ollama models.\n\nBars: 0–100%. History: oldest to newest, up to 60 samples.\n" + app_.commands().chord_text(page_command_) + " pages hosts; " + app_.commands().chord_text(detail_command_) + " pages resource details. Missing metrics are never shown as zero."), ckv::widgets::MessageBoxButtons::Ok});
        box.set_completion_handler([](ckv::widgets::MessageBoxResult) {});
    }});
    for (std::size_t i = 0; i < hosts_.size(); ++i) {
        const auto id = command({.key = "host." + std::to_string(i), .title = i == 0 ? "Local machine" : hosts_[i].endpoint, .handler = [this, i] {
            hosts_[i].want_selected = !hosts_[i].selected; hosts_[i].selected = hosts_[i].want_selected; page_ = 0; save_settings(); changed();
        }});
        app_.commands().set_enabled_predicate(id, [this, i] { return i == 0 || hosts_[i].latest.reachable; });
        app_.commands().set_checked_predicate(id, [this, i] { return hosts_[i].selected; });
        host_commands_.push_back(id);
    }
    shell_ = std::make_unique<ckv::widgets::ApplicationShell>(app_, ckv::widgets::ApplicationShellOptions{
        .theme = settings_.value.dark ? ckv::ui::make_dark_theme(app_.roles(), roles_) : ckv::ui::make_classic_theme(app_.roles(), roles_), .menus = {{"&File", {ckv::widgets::MenuItem::command(app_.commands().standard().quit)}}}, .always_dock_status_line = true});
    app_.commands().set_handler(app_.commands().standard().quit, [this] { app_.request_quit(); });
    auto clock = std::make_unique<ckv::widgets::ClockView>();
    clock->set_time_provider(calendar_.time); clock->set_show_seconds(true);
    clock_ = clock.get();
    header_ = shell_->menu_bar()->set_trailing_view(std::make_unique<MonitorHeader>(std::move(clock), [this] { return host_summary(); }));
    clock_->on_click = [this] { open_calendar(); };
    desktop().set_background_painter([this](ckv::scene::Painter& painter, ckv::Rect area) { paint(painter, area); });
    app_.set_help_provider([this](const std::string&) { app_.execute_command(about_); });
    menus(); changed();
}
void SysmonApp::menus() {
    std::vector<ckv::widgets::MenuItem> hosts;
    for (std::size_t i = 0; i < hosts_.size(); ++i)
        hosts.push_back(ckv::widgets::MenuItem::command(host_commands_[i]).with_disabled_reason(hosts_[i].checked_once ? hosts_[i].latest.diagnostic : "Checking passwordless SSH..."));
    hosts.push_back(ckv::widgets::MenuItem::separator());
    hosts.push_back(ckv::widgets::MenuItem::command(refresh_));
    hosts.push_back(ckv::widgets::MenuItem::command(page_command_));
    hosts.push_back(ckv::widgets::MenuItem::command(detail_command_));
    std::vector<ckv::widgets::MenuItem> metrics;
    for (const auto id : toggles_) metrics.push_back(ckv::widgets::MenuItem::command(id));
    hosts.push_back(ckv::widgets::MenuItem::submenu("&Metrics", std::move(metrics)));
    std::vector<ckv::widgets::MenuItem> themes;
    for (std::size_t i = 0; i < theme_commands_.size(); ++i)
        themes.push_back(ckv::widgets::MenuItem::command(theme_commands_[i]).with_mark_provider([this, i] {
            return settings_.value.dark == (i == 1) ? ckv::widgets::MenuMark::RadioOn : ckv::widgets::MenuMark::RadioOff;
        }));
    hosts.push_back(ckv::widgets::MenuItem::submenu("&Theme", std::move(themes)));
    shell_->menu_bar()->set_menus({
        {"&File", {ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{app_.commands().standard().quit, "E&xit"})}},
        {"&View", std::move(hosts)},
        {"&Help", {ckv::widgets::MenuItem::command(about_), ckv::widgets::MenuItem::command(calendar_command_)}}});
}
void SysmonApp::status() {
    constexpr const char* names[]{"CPU", "RAM", "GPU", "GPU mem", "History", "Ollama"};
    std::vector<ckv::widgets::StatusLineItem> items;
    for (std::size_t i = 0; i < toggles_.size(); ++i)
        items.emplace_back(ckv::widgets::CommandPresentation{toggles_[i], std::string(settings_.value.metrics[i] ? "[x] " : "[ ] ") + names[i]}, i < 4 ? 10 : 5);
    items.emplace_back(ckv::widgets::CommandPresentation{page_command_, "Hosts"}, 2);
    items.emplace_back(ckv::widgets::CommandPresentation{detail_command_, "Details"}, 2);
    shell_->status_line()->set_items(std::move(items));
}
void SysmonApp::changed() { status(); if (header_) header_->invalidate(); desktop().invalidate(); }
void SysmonApp::save_settings() {
    settings_.value.hosts.clear();
    for (const auto& host : hosts_) settings_.value.hosts.emplace(host.endpoint, host.want_selected);
    if (settings_.save) settings_.diagnostic = settings_.save(settings_.value);
}
std::string SysmonApp::host_summary() {
    const auto selected = static_cast<std::size_t>(std::count_if(hosts_.begin(), hosts_.end(), [](const auto& host) { return host.selected; }));
    auto area = desktop().content_area();
    if (!settings_.diagnostic.empty()) { ++area.y; --area.height; }
    const auto grid = host_grid(area, selected);
    auto title = "ck-sysmon / " + std::to_string(selected) + " hosts";
    if (grid.pages > 1) title += " / page " + std::to_string(page_ % grid.pages + 1) + "/" + std::to_string(grid.pages);
    const auto checking = std::count_if(hosts_.begin(), hosts_.end(), [](const auto& h) { return !h.checked_once; });
    const auto unavailable = std::count_if(hosts_.begin() + 1, hosts_.end(), [](const auto& h) { return h.checked_once && !h.latest.reachable; });
    if (checking) title += " / checking " + std::to_string(checking);
    else if (unavailable) title += " / " + std::to_string(unavailable) + " offline";
    return title;
}
void SysmonApp::refresh() { for (const auto& host : hosts_) service_->request(host.endpoint); }
void SysmonApp::accept(std::string endpoint, Sample sample) {
    const auto found = std::find_if(hosts_.begin(), hosts_.end(), [&](const auto& host) { return host.endpoint == endpoint; });
    if (found == hosts_.end()) return;
    const bool menu_changed = !found->checked_once || found->latest.reachable != sample.reachable || found->latest.diagnostic != sample.diagnostic;
    found->accept(std::move(sample));
    if (menu_changed) menus();
    changed();
}
void SysmonApp::open_calendar() {
    if (!calendar_.date) return;
    auto* dropdown = ckv::widgets::show_calendar_dropdown(*clock_, app_, desktop());
    clock_->set_open(true);
    dropdown->on_closed = [clock = clock_] { clock->set_open(false); };
    const auto today = calendar_.date();
    dropdown->show_month(today); dropdown->calendar().set_today(today); dropdown->calendar().set_selected(today);
}
void SysmonApp::paint(ckv::scene::Painter& painter, ckv::Rect area) {
    const auto colors = chart_palette(settings_.value.dark);
    painter.fill(area, ckv::Cell::from_grapheme(" ", colors.background));
    if (!settings_.diagnostic.empty()) {
        painter.draw_text({area.x + 1, area.y}, ckv::text::elide_to_width(settings_.diagnostic, std::max(0, area.width - 2)), colors.amber);
        ++area.y; --area.height;
    }
    if (area.width < 26 || area.height < 8) {
        painter.draw_text({area.x, area.y}, "ck-sysmon: enlarge terminal", colors.heading); return;
    }
    std::vector<const HostState*> selected;
    for (const auto& host : hosts_) if (host.selected) selected.push_back(&host);
    if (selected.empty()) {
        painter.draw_text({area.x + 1, area.y}, "No hosts selected", colors.heading);
        painter.draw_text({area.x + 1, area.y + 2}, "Select hosts in the View menu.", colors.quiet);
        return;
    }
    const auto [columns, rows, per_page, pages] = host_grid(area, selected.size());
    const auto page = page_ % pages;
    const int width = (area.width - 2 - (columns - 1)) / columns;
    for (std::size_t slot = 0; slot < per_page && page * per_page + slot < selected.size(); ++slot) {
        const int col = static_cast<int>(slot % static_cast<std::size_t>(columns));
        const int row = static_cast<int>(slot / static_cast<std::size_t>(columns));
        const int top = area.height * row / rows;
        const int bottom = area.height * (row + 1) / rows;
        paint_host(painter, {area.x + 1 + col * (width + 1), area.y + top, width, bottom - top}, *selected[page * per_page + slot]);
    }
}
void SysmonApp::paint_host(ckv::scene::Painter& painter, ckv::Rect card, const HostState& host) {
    const auto colors = chart_palette(settings_.value.dark);
    painter.draw_box(card, ckv::scene::LineStyle::Single, colors.frame);
    painter.draw_text({card.x + 2, card.y}, ckv::text::elide_to_width(
        " " + (host.endpoint.empty() ? "LOCAL" : host.endpoint) + " ", card.width - 4), colors.heading);
    auto clipped = painter.clipped({card.x + 2, card.y + 1, card.width - 4, card.height - 2});
    const int x = card.x + 2, width = card.width - 4;
    int y = card.y + 1;
    clipped.draw_text({x, y++}, host.checked_once ? host.latest.system : "Collecting native metrics...", colors.quiet);
    if (!host.latest.diagnostic.empty()) clipped.draw_text({x, y++}, host.latest.diagnostic, colors.amber);
    std::vector<Row> rows;
    if (settings_.value.metrics[0]) rows.push_back({"CPU", host.latest.cpu && !host.latest.cpu_load ? "sampling interval..." : "", host.latest.cpu_load, &host.cpu_history});
    if (settings_.value.metrics[1]) rows.push_back({"RAM", memory_text(host.latest.memory_used, host.latest.memory_total), percent(host.latest.memory_used, host.latest.memory_total), &host.memory_history});
    if (settings_.value.metrics[2] || settings_.value.metrics[3]) {
        if (host.latest.gpus.empty()) rows.push_back({"GPU", "not reported / no supported device", {}, nullptr, false});
        for (std::size_t i = 0; i < host.latest.gpus.size(); ++i) {
            const auto& gpu = host.latest.gpus[i];
            const auto history = std::find_if(host.gpu_history.begin(), host.gpu_history.end(), [&](const auto& h) { return h.id == gpu.id; });
            auto name = "GPU " + std::to_string(i) + "  " + gpu.name;
            rows.push_back({name, "", {}, nullptr, false});
            if (settings_.value.metrics[2]) {
                std::string detail;
                if (gpu.temperature) detail += decimal(*gpu.temperature) + " °C";
                if (gpu.watts) detail += "  " + decimal(*gpu.watts) + " W";
                rows.push_back({"G" + std::to_string(i) + " use", (gpu.memory_kind == GpuMemoryKind::V3dBuffers ? "busiest GPU engine" : detail), gpu.load, history == host.gpu_history.end() ? nullptr : &history->load});
            }
            if (settings_.value.metrics[3]) {
                auto detail = memory_text(gpu.used, gpu.total);
                if (gpu.memory_kind == GpuMemoryKind::UnifiedDriver) {
                    detail = gpu.used && !gpu.total ? decimal(static_cast<double>(*gpu.used) / 1073741824.0) + " GiB" : detail;
                    detail += " unified RAM (driver)";
                }
                if (gpu.memory_kind == GpuMemoryKind::V3dBuffers) {
                    detail = gpu.used ? decimal(static_cast<double>(*gpu.used) / 1048576.0) + " MiB GPU buffers (V3D)" : "GPU buffers (V3D) not reported";
                    rows.push_back({"G" + std::to_string(i) + " buffers", detail, {}, nullptr, false, true});
                    continue;
                }
                if (gpu.shared) detail += "  +" + decimal(static_cast<double>(*gpu.shared) / 1073741824.0) + " GiB shared RAM";
                rows.push_back({"G" + std::to_string(i) + (gpu.memory_kind == GpuMemoryKind::UnifiedDriver ? " RAM" : " mem"), detail, percent(gpu.used, gpu.total), history == host.gpu_history.end() ? nullptr : &history->memory});
            }
        }
    }
    if (settings_.value.metrics[5]) {
        rows.push_back({"Ollama: " + host.latest.ollama_status + (host.latest.ollama_status == "running" && host.latest.models.empty() ? " / no loaded models" : ""), "", {}, nullptr, false});
        for (const auto& model : host.latest.models)
            rows.push_back({model.name, model.vram ? decimal(static_cast<double>(*model.vram) / 1073741824.0) + " GiB GPU" : "GPU n/a", {}, nullptr, false, true});
    }
    if (rows.empty()) rows.push_back({"All metrics hidden", "Use the status toggles to show metrics", {}, nullptr, false});
    const int capacity = std::max(1, card.y + card.height - 2 - y);
    std::vector<std::vector<const Row*>> pages(1);
    int consumed = 0;
    for (const auto& row : rows) {
        const int height = row.compact || row.detail.empty() ? 1 : 2;
        if (consumed && consumed + height > capacity) { pages.emplace_back(); consumed = 0; }
        pages.back().push_back(&row); consumed += height;
    }
    const auto page = detail_page_ % pages.size();
    for (const auto* entry : pages[page]) {
        const auto& row = *entry;
        if (!row.metric) {
            if (row.compact) {
                const int detail_width = ckv::text::text_width(row.detail);
                clipped.draw_text({x, y}, ckv::text::elide_to_width(row.label, std::max(1, width - detail_width - 2)), colors.heading);
                clipped.draw_text({x + width - detail_width, y++}, row.detail, colors.quiet);
            } else {
                clipped.draw_text({x, y++}, row.label, colors.heading);
                if (!row.detail.empty()) clipped.draw_text({x, y++}, row.detail, colors.quiet);
            }
            continue;
        }
        const ckv::Style accent = row.value && *row.value >= 90 ? colors.red : row.value && *row.value >= 70 ? colors.amber : colors.green;
        const int history_width = settings_.value.metrics[4] && width >= 40 ? std::min(16, width / 4) : 0;
        const int bar_width = std::max(1, width - 17 - history_width);
        clipped.draw_text({x, y}, row.label, colors.heading);
        clipped.draw_text({x + 7, y}, "[" + std::string(static_cast<std::size_t>(bar_width), ' ') + "]", colors.frame);
        const int fill = row.value ? static_cast<int>(std::clamp(*row.value, 0.0, 100.0) * bar_width / 100.0) : 0;
        for (int i = 0; i < bar_width; ++i) clipped.draw_text({x + 8 + i, y}, i < fill ? "█" : "·", i < fill ? accent : colors.frame);
        clipped.draw_text({x + 10 + bar_width, y}, row.value ? decimal(*row.value) + "%" : "n/a", row.value ? accent : colors.quiet);
        if (history_width && row.history) clipped.draw_text({x + width - history_width, y}, sparkline(*row.history, history_width), accent);
        ++y;
        if (!row.detail.empty()) clipped.draw_text({x + 6, y++}, row.detail, colors.quiet);
    }
    if (pages.size() > 1) painter.draw_text({card.x + 2, card.y + card.height - 1}, " " + app_.commands().chord_text(detail_command_) + " details " + std::to_string(page + 1) + "/" + std::to_string(pages.size()) + " ", colors.amber);
}
} // namespace ck::sysmon
