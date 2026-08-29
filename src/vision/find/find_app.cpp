#include "find_app.hpp"

#include <sstream>
#include <utility>
#include <vector>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/text_layout.hpp>

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/search_backend.hpp"

namespace ck::vision
{
namespace
{
using ckv::ui::CommandDescriptor;
using ckv::ui::CommandVisibility;
using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;
}

FindApp::FindApp(ckv::ui::Application &application)
    : application_(application), specification_(ck::find::makeDefaultSpecification())
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    show_preview();
}

void FindApp::declare_commands()
{
    new_search_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.find.new_search", .title = "&New search...", .category = "Find", .chord = "Ctrl+N",
        .visibility = CommandVisibility::Palette, .handler = [this] { show_search_dialog(); }});
    preview_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.find.preview_command", .title = "&Preview command", .category = "Find", .chord = "F5",
        .visibility = CommandVisibility::Palette, .handler = [this] { show_preview(); }});
}

SuiteShellOptions FindApp::make_shell_options() const
{
    return {.application_name = "ck Find",
            .about_text = "A native ckVision front end for building and previewing reusable file-search specifications.",
            .application_menus = {MenuBarItem{"&Search", {
                MenuItem::command(CommandPresentation{new_search_command_, "&New search..."}),
                MenuItem::command(CommandPresentation{preview_command_, "&Preview command"}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{new_search_command_, "&New"}, 30},
                StatusLineItem{CommandPresentation{preview_command_, "&Preview"}, 25},
            }};
}

void FindApp::show_search_dialog()
{
    search_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "New search";
    dialog.fields.push_back({"&Start location:", ck::find::bufferToString(specification_.startLocation),
                             [](const std::string &value) { return !value.empty(); }});
    dialog.fields.push_back({"Search &text:", ck::find::bufferToString(specification_.searchText), nullptr});
    ckv::widgets::FieldDescriptor recursive;
    recursive.label = "Search &subdirectories";
    recursive.kind = ckv::widgets::FieldKind::Check;
    recursive.initial_checked = specification_.includeSubdirectories;
    dialog.fields.push_back(std::move(recursive));
    ckv::widgets::FieldDescriptor hidden;
    hidden.label = "Include &hidden files";
    hidden.kind = ckv::widgets::FieldKind::Check;
    hidden.initial_checked = specification_.includeHidden;
    dialog.fields.push_back(std::move(hidden));
    dialog.buttons.push_back({"&Apply", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    search_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    search_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.size() < 4 || result.checked.size() < 4)
            return;
        ck::find::copyToArray(specification_.startLocation, result.values[0].c_str());
        ck::find::copyToArray(specification_.searchText, result.values[1].c_str());
        specification_.includeSubdirectories = result.checked[2];
        specification_.includeHidden = result.checked[3];
        specification_.enableTextSearch = !result.values[1].empty();
        show_preview();
    });
}

std::string FindApp::command_preview() const
{
    const auto command = ck::find::buildFindCommand(specification_, true);
    std::ostringstream output;
    for (std::size_t index = 0; index < command.size(); ++index)
    {
        if (index != 0)
            output << ' ';
        output << command[index];
    }
    return output.str();
}

void FindApp::show_preview()
{
    present_text_window("Find command preview", command_preview());
}

void FindApp::present_text_window(std::string title, std::string content)
{
    auto window = std::make_unique<ckv::widgets::Window>(std::move(title));
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{50, 10});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    auto text = std::make_unique<ckv::widgets::TextView>();
    text->set_wrap_mode(ckv::widgets::WrapMode::Word);
    text->set_text(std::move(content));
    window->set_content(std::move(text));
    shell_->desktop().add_window(std::move(window));
}

} // namespace ck::vision
