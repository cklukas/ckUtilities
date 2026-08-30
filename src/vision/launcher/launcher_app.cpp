#include "launcher_app.hpp"

#include "calculator_model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#include <cvision/core/diagnostics.hpp>
#include <cvision/ui/grid.hpp>
#include <cvision/widgets/button.hpp>
#include <cvision/widgets/common_components.hpp>
#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/input_line.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/splitter.hpp>

#include "ck/vision/keymap.hpp"
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/text_layout.hpp>

namespace ck::vision
{
namespace
{

using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;

std::string ascii_character(unsigned char value)
{
    if (value == 0)
        return "NUL";
    if (value == 9)
        return "TAB";
    if (value == 10)
        return "LF";
    if (value == 13)
        return "CR";
    if (value == 27)
        return "ESC";
    if (value == 32)
        return "SPACE";
    if (value >= 33 && value < 127)
        return std::string(1, static_cast<char>(value));
    if (value == 127)
        return "DEL";
    return "non-printing";
}

std::vector<std::vector<std::string>> ascii_rows()
{
    std::vector<std::vector<std::string>> rows;
    rows.reserve(256);
    for (unsigned int value = 0; value < 256; ++value)
    {
        std::ostringstream hex;
        hex << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << value;
        rows.push_back({std::to_string(value), hex.str(), ascii_character(static_cast<unsigned char>(value))});
    }
    return rows;
}

const std::vector<std::string> &palette_names()
{
    static const std::vector<std::string> names = {
        "Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "Light gray",
        "Dark gray", "Light blue", "Light green", "Light cyan", "Light red", "Light magenta",
        "Yellow", "White",
    };
    return names;
}

std::string_view palette_name(int index)
{
    const auto &names = palette_names();
    if (index < 0 || index >= static_cast<int>(names.size()))
        return "Unknown";
    return names[static_cast<std::size_t>(index)];
}

struct CalculatorWindowState
{
    ckv::widgets::InputLine *input = nullptr;

    void append(std::string_view text) const
    {
        if (input != nullptr)
            input->set_text(input->text() + std::string(text));
    }

    void erase_last() const
    {
        if (input == nullptr)
            return;
        std::string text = input->text();
        if (!text.empty())
            text.pop_back();
        input->set_text(std::move(text));
    }

    void clear() const
    {
        if (input != nullptr)
            input->set_text({});
    }

    void evaluate() const
    {
        if (input == nullptr)
            return;
        input->set_text(CalculatorModel::evaluate(input->text()).text);
    }
};

class LauncherDiagnosticsSink final : public ckv::DiagnosticsSink
{
public:
    explicit LauncherDiagnosticsSink(std::shared_ptr<BoundedDiagnostics> log) : log_(std::move(log)) {}

    void log(ckv::LogLevel level, std::string_view message) noexcept override;

private:
    std::shared_ptr<BoundedDiagnostics> log_;
};

} // namespace

class BoundedDiagnostics
{
public:
    static constexpr std::size_t kMaximumEntries = 200;

    void append(ckv::LogLevel level, std::string_view message) noexcept
    {
        try
        {
            if (entries_.size() == kMaximumEntries)
                entries_.erase(entries_.begin());
            entries_.push_back({level, std::string(message)});
        }
        catch (...)
        {
            // Diagnostics are best effort: an allocation failure may discard
            // an observation but must never disturb the application itself.
        }
    }

    std::size_t size() const noexcept { return entries_.size(); }

    std::string snapshot() const
    {
        std::ostringstream output;
        for (const Entry &entry : entries_)
            output << level_name(entry.level) << ": " << entry.message << '\n';
        return output.str();
    }

private:
    struct Entry
    {
        ckv::LogLevel level;
        std::string message;
    };

    static std::string_view level_name(ckv::LogLevel level) noexcept
    {
        switch (level)
        {
        case ckv::LogLevel::Trace: return "trace";
        case ckv::LogLevel::Debug: return "debug";
        case ckv::LogLevel::Info: return "info";
        case ckv::LogLevel::Warning: return "warning";
        case ckv::LogLevel::Error: return "error";
        }
        return "unknown";
    }

    std::vector<Entry> entries_;
};

void LauncherDiagnosticsSink::log(ckv::LogLevel level, std::string_view message) noexcept
{
    if (log_ != nullptr)
        log_->append(level, message);
}

UtilitiesLauncherApp::UtilitiesLauncherApp(ckv::ui::Application &application, LaunchHandler on_launch)
    : application_(application), on_launch_(std::move(on_launch)), command_scope_(application.commands())
{
    diagnostics_ = std::make_shared<BoundedDiagnostics>();
    application_.set_diagnostics_sink(std::make_unique<LauncherDiagnosticsSink>(diagnostics_));
    log_diagnostic(ckv::LogLevel::Info, "CK Utilities native launcher started");
    for (const ck::appinfo::ToolInfo &tool : ck::appinfo::tools())
    {
        if (tool.id != "ck-utilities")
            tools_.push_back(&tool);
    }
    std::sort(tools_.begin(), tools_.end(), [](const auto *left, const auto *right) {
        return left->displayName < right->displayName;
    });

    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    open_launcher_window();
}

void UtilitiesLauncherApp::declare_commands()
{
    launch_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.launch_tool", [this] { launch_active_tool(); }));
    new_launcher_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.new_launcher", [this] { open_launcher_window(); }));
    calendar_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.show_calendar", [this] { open_calendar_window(); }));
    ascii_table_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.show_ascii_table", [this] { open_ascii_table_window(); }));
    calculator_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.show_calculator", [this] { open_calculator_window(); }));
    diagnostics_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.show_diagnostics", [this] { open_diagnostics_window(); }));
    color_selector_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-utilities", "ck.utilities.show_color_selector", [this] { open_color_selector(); }));
}

SuiteShellOptions UtilitiesLauncherApp::make_shell_options() const
{
    return {
        .application_name = "CK Utilities",
        .about_text = "The native ckVision suite launcher. Browse installed CK tools, inspect their descriptions, and launch the selected tool in a fresh terminal session.",
        .application_menus = {
            MenuBarItem{"&Tools", {
                MenuItem::command(CommandPresentation{launch_command_, "&Launch selected tool"}),
                MenuItem::command(CommandPresentation{calendar_command_, "Show &Calendar"}),
                MenuItem::command(CommandPresentation{ascii_table_command_, "Show &ASCII table"}),
                MenuItem::command(CommandPresentation{calculator_command_, "Show &Calculator"}),
                MenuItem::command(CommandPresentation{diagnostics_command_, "Show &Diagnostics"}),
                MenuItem::command(CommandPresentation{color_selector_command_, "Show color &selector"}),
            }},
            MenuBarItem{"&Window", {MenuItem::command(CommandPresentation{new_launcher_command_, "&New launcher window"})}},
        },
        .application_status_items = {
            StatusLineItem{CommandPresentation{launch_command_, "&Launch"}, 30},
            StatusLineItem{CommandPresentation{calendar_command_, "&Calendar"}, 25},
            StatusLineItem{CommandPresentation{ascii_table_command_, "&ASCII"}, 20},
            StatusLineItem{CommandPresentation{calculator_command_, "&Calculator"}, 20},
            StatusLineItem{CommandPresentation{diagnostics_command_, "&Diagnostics"}, 20},
            StatusLineItem{CommandPresentation{color_selector_command_, "&Colors"}, 20},
            StatusLineItem{CommandPresentation{new_launcher_command_, "&New"}, 20},
        },
    };
}

void UtilitiesLauncherApp::open_launcher_window()
{
    log_diagnostic(ckv::LogLevel::Info, "Opened launcher window");
    auto state = std::make_unique<LauncherWindow>();
    LauncherWindow *const raw_state = state.get();

    auto window = std::make_unique<ckv::widgets::Window>("CK Utilities");
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{60, 16});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->on_closed = [this, raw_state] { close_launcher_window(raw_state); };

    auto tool_list = std::make_unique<ckv::widgets::ListView>();
    state->tool_list = tool_list.get();
    std::vector<std::string> tool_names;
    tool_names.reserve(tools_.size());
    for (const ck::appinfo::ToolInfo *tool : tools_)
        tool_names.emplace_back(tool->displayName);
    state->tool_list->set_items(std::move(tool_names));
    state->tool_list->on_cursor_changed = [this, raw_state](std::size_t index) {
        update_detail(*raw_state, index);
    };
    state->tool_list->on_activate = [this](std::size_t) { launch_active_tool(); };

    auto detail = std::make_unique<ckv::widgets::TextView>();
    state->detail = detail.get();
    state->detail->set_wrap_mode(ckv::widgets::WrapMode::Word);

    auto splitter = std::make_unique<ckv::widgets::Splitter>(window->content_rect(),
                                                               std::move(tool_list), std::move(detail));
    window->set_content(std::move(splitter));
    state->window = shell_->desktop().add_window(std::move(window));
    windows_.push_back(std::move(state));

    if (!tools_.empty())
    {
        raw_state->tool_list->set_cursor(0);
        application_.set_focus(raw_state->tool_list);
    }
}

void UtilitiesLauncherApp::update_detail(LauncherWindow &window, std::size_t tool_index)
{
    if (window.detail == nullptr || tool_index >= tools_.size())
        return;

    const ck::appinfo::ToolInfo &tool = *tools_[tool_index];
    window.detail->set_text(std::string(tool.displayName) + "\n\n" +
                            std::string(tool.shortDescription) + "\n\n" +
                            std::string(tool.longDescription));
    if (window.window != nullptr)
        window.window->set_footer(std::string(tool.executable));
}

void UtilitiesLauncherApp::launch_active_tool()
{
    const ck::appinfo::ToolInfo *tool = selected_tool();
    if (tool == nullptr)
        return;
    log_diagnostic(ckv::LogLevel::Info, std::string("Launching ") + std::string(tool->id));
    if (on_launch_)
        on_launch_(*tool);
    application_.request_quit();
}

void UtilitiesLauncherApp::open_calendar_window()
{
    log_diagnostic(ckv::LogLevel::Info, "Opened calendar");
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::chrono::year_month_day date{today};
    const ckv::widgets::DateValue selected{
        static_cast<int>(date.year()), static_cast<int>(static_cast<unsigned>(date.month())),
        static_cast<int>(static_cast<unsigned>(date.day()))};

    auto window = std::make_unique<ckv::widgets::Window>("Calendar");
    window->set_min_size(ckv::Size{32, 12});
    auto calendar = std::make_unique<ckv::widgets::CalendarView>();
    calendar->set_selected(selected);
    calendar->set_today(selected);
    window->set_content(std::move(calendar));
    shell_->desktop().add_window(std::move(window));
}

void UtilitiesLauncherApp::open_ascii_table_window()
{
    log_diagnostic(ckv::LogLevel::Info, "Opened ASCII table");
    auto window = std::make_unique<ckv::widgets::Window>("ASCII table");
    window->set_min_size(ckv::Size{42, 14});
    window->set_footer("Navigate cells with arrows; values are decimal, hexadecimal, and printable names.");

    auto table = std::make_unique<ckv::widgets::Table>();
    table->set_columns({
        {"Decimal", 9, 3},
        {"Hex", 8, 3},
        {"Character", 18, 5},
    });
    table->set_rows(ascii_rows());
    window->set_content(std::move(table));
    shell_->desktop().add_window(std::move(window));
}

void UtilitiesLauncherApp::open_calculator_window()
{
    log_diagnostic(ckv::LogLevel::Info, "Opened calculator");
    auto window = std::make_unique<ckv::widgets::Window>("Calculator");
    window->set_min_size(ckv::Size{32, 15});
    window->set_footer("Enter evaluates; expressions support +, -, *, /, parentheses, and %. ");

    auto grid = std::make_unique<ckv::ui::Grid>(ckv::Rect{0, 0, 30, 13}, 6, 4);
    auto state = std::make_shared<CalculatorWindowState>();
    auto input = std::make_unique<ckv::widgets::InputLine>();
    state->input = static_cast<ckv::widgets::InputLine *>(
        grid->add_item(std::move(input), ckv::ui::GridSpec{.row = 0, .column = 0, .column_span = 4}));
    state->input->on_accept = [state] { state->evaluate(); };

    static constexpr std::array<std::string_view, 20> keys = {
        "C", "<", "%", "/",
        "7", "8", "9", "*",
        "4", "5", "6", "-",
        "1", "2", "3", "+",
        "0", ".", "(", "=",
    };
    for (std::size_t index = 0; index < keys.size(); ++index)
    {
        const std::string_view key = keys[index];
        auto button = std::make_unique<ckv::widgets::Button>(std::string(key));
        button->set_flat(true);
        button->on_press = [state, key] {
            if (key == "C")
                state->clear();
            else if (key == "<")
                state->erase_last();
            else if (key == "=")
                state->evaluate();
            else
                state->append(key);
        };
        grid->add_item(std::move(button), ckv::ui::GridSpec{
            .row = static_cast<int>(index / 4) + 1,
            .column = static_cast<int>(index % 4),
        });
    }

    window->set_content(std::move(grid));
    shell_->desktop().add_window(std::move(window));
}

void UtilitiesLauncherApp::open_diagnostics_window()
{
    log_diagnostic(ckv::LogLevel::Info, "Opened diagnostics window");
    auto window = std::make_unique<ckv::widgets::Window>("Application diagnostics");
    window->set_min_size(ckv::Size{52, 14});
    window->set_footer("Bounded application diagnostics captured through ckVision's injected diagnostics sink.");
    auto text = std::make_unique<ckv::widgets::TextView>();
    text->set_wrap_mode(ckv::widgets::WrapMode::Word);
    text->set_text(diagnostics_->snapshot());
    window->set_content(std::move(text));
    shell_->desktop().add_window(std::move(window));
}

void UtilitiesLauncherApp::open_color_selector()
{
    log_diagnostic(ckv::LogLevel::Info, "Opened color selector");
    color_dialog_.reset();

    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Color selector";
    dialog.minimum_window_size = ckv::Size{48, 16};
    dialog.resizable = true;
    dialog.fields.push_back({
        .label = "&Background",
        .kind = ckv::widgets::FieldKind::Radio,
        .options = palette_names(),
        .initial_selection = background_color_,
    });
    dialog.fields.push_back({
        .label = "&Foreground",
        .kind = ckv::widgets::FieldKind::Radio,
        .options = palette_names(),
        .initial_selection = foreground_color_,
    });
    dialog.buttons.push_back({"&Apply", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    color_dialog_.emplace(
        ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    color_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.size() != 2 || result.selected[0] < 0 || result.selected[1] < 0)
            return;
        background_color_ = result.selected[0];
        foreground_color_ = result.selected[1];
        log_diagnostic(ckv::LogLevel::Info, "Applied color selection");
        show_color_selection();
    });
}

void UtilitiesLauncherApp::show_color_selection()
{
    auto window = std::make_unique<ckv::widgets::Window>("Selected colors");
    window->set_min_size(ckv::Size{40, 10});
    window->set_footer("The color choices are preserved for the current launcher session.");
    auto text = std::make_unique<ckv::widgets::TextView>();
    text->set_text("Background: " + std::string(palette_name(background_color_)) + " (" +
                   std::to_string(background_color_) + ")\n\nForeground: " +
                   std::string(palette_name(foreground_color_)) + " (" + std::to_string(foreground_color_) + ")");
    window->set_content(std::move(text));
    shell_->desktop().add_window(std::move(window));
}

void UtilitiesLauncherApp::log_diagnostic(ckv::LogLevel level, std::string_view message) noexcept
{
    application_.diagnostics().log(level, message);
}

std::size_t UtilitiesLauncherApp::diagnostics_entry_count() const noexcept
{
    return diagnostics_ == nullptr ? 0 : diagnostics_->size();
}

void UtilitiesLauncherApp::close_launcher_window(LauncherWindow *window)
{
    const auto found = std::find_if(windows_.begin(), windows_.end(), [window](const auto &candidate) {
        return candidate.get() == window;
    });
    if (found == windows_.end())
        return;

    ckv::widgets::Window *const owned_window = (*found)->window;
    windows_.erase(found);
    if (owned_window != nullptr)
        shell_->desktop().remove_window(owned_window);
    if (windows_.empty())
        application_.request_quit();
}

UtilitiesLauncherApp::LauncherWindow *UtilitiesLauncherApp::active_launcher_window() const noexcept
{
    const ckv::widgets::Window *const active = shell_->desktop().active_window();
    const auto found = std::find_if(windows_.begin(), windows_.end(), [active](const auto &candidate) {
        return candidate->window == active;
    });
    if (found != windows_.end())
        return found->get();

    // A utility window may temporarily be active while a menu command still
    // refers to the selected launcher tool. Retain the most recently opened
    // launcher as that command's stable context rather than making Launch a
    // silent no-op solely because the reader checked the calendar first.
    return windows_.empty() ? nullptr : windows_.back().get();
}

const ck::appinfo::ToolInfo *UtilitiesLauncherApp::selected_tool() const noexcept
{
    const LauncherWindow *const active = active_launcher_window();
    if (active == nullptr || active->tool_list == nullptr)
        return nullptr;
    const int cursor = active->tool_list->cursor();
    if (cursor < 0 || cursor >= static_cast<int>(tools_.size()))
        return nullptr;
    return tools_[static_cast<std::size_t>(cursor)];
}

} // namespace ck::vision
