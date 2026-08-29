#include "config_app.hpp"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>

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

std::string option_value_text(const ck::config::OptionDefinition &definition, const ck::config::OptionValue &value)
{
    switch (definition.kind)
    {
    case ck::config::OptionKind::Boolean:
        return value.toBool() ? "true" : "false";
    case ck::config::OptionKind::Integer:
    case ck::config::OptionKind::String:
        return value.toString();
    case ck::config::OptionKind::StringList:
    {
        std::ostringstream output;
        const auto values = value.toStringList();
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index != 0)
                output << ", ";
            output << values[index];
        }
        return output.str();
    }
    }
    return {};
}

std::string kind_name(ck::config::OptionKind kind)
{
    switch (kind)
    {
    case ck::config::OptionKind::Boolean: return "Boolean";
    case ck::config::OptionKind::Integer: return "Integer";
    case ck::config::OptionKind::String: return "Text";
    case ck::config::OptionKind::StringList: return "Text list";
    }
    return "Unknown";
}

std::vector<std::string> split_list(std::string_view value)
{
    std::vector<std::string> result;
    std::stringstream input{std::string(value)};
    std::string item;
    while (std::getline(input, item, ','))
    {
        const auto first = item.find_first_not_of(" \t");
        const auto last = item.find_last_not_of(" \t");
        if (first != std::string::npos)
            result.push_back(item.substr(first, last - first + 1));
    }
    return result;
}

} // namespace

class OptionTableModel final : public ckv::widgets::TableModel
{
public:
    explicit OptionTableModel(ck::config::OptionRegistry &registry) : registry_(registry) { refresh(); }

    void refresh() { definitions_ = registry_.listRegisteredOptions(); }
    std::size_t row_count() const override { return definitions_.size(); }
    ckv::widgets::TableRowId row_id_at(std::size_t index) const override
    {
        return index < definitions_.size() ? static_cast<ckv::widgets::TableRowId>(index + 1)
                                           : ckv::widgets::kInvalidTableRowId;
    }
    std::optional<std::size_t> index_of(ckv::widgets::TableRowId id) const override
    {
        if (id == ckv::widgets::kInvalidTableRowId || id > definitions_.size())
            return std::nullopt;
        return static_cast<std::size_t>(id - 1);
    }
    ckv::widgets::TableCell cell(ckv::widgets::TableCellRef reference) const override
    {
        const auto index = index_of(reference.row);
        if (!index || reference.column >= 4)
            return {};
        const auto &definition = definitions_[*index];
        switch (reference.column)
        {
        case 0: return {definition.displayName, definition.displayName, std::nullopt, false};
        case 1: return {definition.key, definition.key, std::nullopt, false};
        case 2: return {kind_name(definition.kind), kind_name(definition.kind), std::nullopt, false};
        case 3:
            return {option_value_text(definition, registry_.get(definition.key)),
                    option_value_text(definition, registry_.get(definition.key)), std::nullopt, false};
        }
        return {};
    }
    const ck::config::OptionDefinition *definition(ckv::widgets::TableRowId id) const
    {
        const auto index = index_of(id);
        return index ? &definitions_[*index] : nullptr;
    }
    std::optional<ckv::widgets::TableRowId> id_for_key(std::string_view key) const
    {
        const auto found = std::find_if(definitions_.begin(), definitions_.end(), [key](const auto &definition) {
            return definition.key == key;
        });
        if (found == definitions_.end())
            return std::nullopt;
        return static_cast<ckv::widgets::TableRowId>(std::distance(definitions_.begin(), found) + 1);
    }

private:
    ck::config::OptionRegistry &registry_;
    std::vector<ck::config::OptionDefinition> definitions_;
};

ConfigApp::~ConfigApp() = default;

ConfigApp::ConfigApp(ckv::ui::Application &application,
                     ck::config::OptionRegistry &registry,
                     ConfigPersistence &persistence)
    : application_(application), registry_(registry), persistence_(persistence), model_(std::make_unique<OptionTableModel>(registry_))
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_window();
}

void ConfigApp::declare_commands()
{
    edit_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.config.edit_selected", .title = "&Edit selected option", .category = "Configuration",
        .chord = "Enter", .visibility = CommandVisibility::Palette, .handler = [this] { edit_selected(); }});
    reset_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.config.reset_selected", .title = "&Reset selected option", .category = "Configuration",
        .visibility = CommandVisibility::Palette, .handler = [this] { reset_selected(); }});
    save_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.config.save", .title = "&Save configuration", .category = "Configuration", .chord = "Ctrl+S",
        .visibility = CommandVisibility::Palette, .handler = [this] { save(); }});
    reload_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.config.reload", .title = "&Reload saved configuration", .category = "Configuration", .chord = "Ctrl+R",
        .visibility = CommandVisibility::Palette, .handler = [this] { reload(); }});
}

SuiteShellOptions ConfigApp::make_shell_options() const
{
    return {.application_name = "ck Config",
            .about_text = "A native ckVision editor for an injected option registry and persistence policy.",
            .application_menus = {MenuBarItem{"&Options", {
                MenuItem::command(CommandPresentation{edit_command_, "&Edit selected option"}),
                MenuItem::command(CommandPresentation{reset_command_, "&Reset selected option"}),
                MenuItem::command(CommandPresentation{save_command_, "&Save configuration"}),
                MenuItem::command(CommandPresentation{reload_command_, "&Reload saved configuration"}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{edit_command_, "&Edit"}, 20},
                StatusLineItem{CommandPresentation{reset_command_, "&Reset"}, 20},
                StatusLineItem{CommandPresentation{save_command_, "&Save"}, 20},
                StatusLineItem{CommandPresentation{reload_command_, "&Reload"}, 25},
            }};
}

void ConfigApp::create_window()
{
    auto window = std::make_unique<ckv::widgets::Window>("Configuration: " + registry_.appId());
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{70, 16});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->on_closed = [this] { application_.request_quit(); };
    auto table = std::make_unique<ckv::widgets::Table>();
    table_ = table.get();
    table_->set_columns({{"Name", 25, 10}, {"Key", 25, 10}, {"Type", 12, 6}, {"Value", 30, 10}});
    table_->set_model(*model_);
    table_->on_selection_changed = [this](ckv::widgets::TableCellRef reference) {
        if (const auto *definition = model_->definition(reference.row))
            selected_key_ = definition->key;
    };
    window->set_content(std::move(table));
    window_ = shell_->desktop().add_window(std::move(window));
    refresh();
    application_.set_focus(table_);
}

void ConfigApp::refresh()
{
    model_->refresh();
    if (table_ == nullptr)
        return;
    table_->model_changed();
    if (selected_key_.empty() && option_count() != 0)
        selected_key_ = model_->definition(1)->key;
    if (const auto id = model_->id_for_key(selected_key_))
        table_->set_selected_cell({*id, 0});
    set_status(std::to_string(option_count()) + " registered options.");
}

bool ConfigApp::select_option(std::string_view key)
{
    const auto id = model_->id_for_key(key);
    if (!id || table_ == nullptr)
        return false;
    selected_key_ = std::string(key);
    table_->set_selected_cell({*id, 0});
    return true;
}

const ck::config::OptionDefinition *ConfigApp::selected_definition() const
{
    return selected_key_.empty() ? nullptr : registry_.definition(selected_key_);
}

void ConfigApp::edit_selected()
{
    const auto *definition = selected_definition();
    if (definition == nullptr)
        return;
    edit_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Edit " + definition->displayName;
    dialog.fields.push_back({"", definition->description, nullptr, false, '*', ckv::widgets::FieldKind::Note});
    switch (definition->kind)
    {
    case ck::config::OptionKind::Boolean:
        dialog.fields.push_back({"&Value", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                                 {"false", "true"}, registry_.get(definition->key).toBool() ? 1 : 0});
        break;
    case ck::config::OptionKind::Integer:
        dialog.fields.push_back({"&Value", registry_.get(definition->key).toString(), nullptr, false, '*',
                                 ckv::widgets::FieldKind::Number});
        break;
    case ck::config::OptionKind::String:
        dialog.fields.push_back({"&Value", registry_.get(definition->key).toString(), nullptr});
        break;
    case ck::config::OptionKind::StringList:
        dialog.fields.push_back({"Comma-separated &values", option_value_text(*definition, registry_.get(definition->key)), nullptr});
        break;
    }
    dialog.buttons.push_back({"&Apply", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    const std::string key = definition->key;
    const ck::config::OptionKind kind = definition->kind;
    edit_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    edit_dialog_->set_completion_handler([this, key, kind](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.size() < 2 || result.selected.size() < 2 || result.numbers.size() < 2)
            return;
        switch (kind)
        {
        case ck::config::OptionKind::Boolean: registry_.set(key, ck::config::OptionValue(result.selected[1] == 1)); break;
        case ck::config::OptionKind::Integer:
            if (result.numbers[1]) registry_.set(key, ck::config::OptionValue(static_cast<std::int64_t>(*result.numbers[1])));
            break;
        case ck::config::OptionKind::String: registry_.set(key, ck::config::OptionValue(result.values[1])); break;
        case ck::config::OptionKind::StringList: registry_.set(key, ck::config::OptionValue(split_list(result.values[1]))); break;
        }
        refresh();
    });
}

void ConfigApp::reset_selected()
{
    if (const auto *definition = selected_definition())
    {
        registry_.reset(definition->key);
        refresh();
    }
}

void ConfigApp::save()
{
    if (persistence_.save(registry_))
        set_status("Saved configuration for " + registry_.appId() + ".");
    else
        set_status("Could not save configuration for " + registry_.appId() + ".");
}

void ConfigApp::reload()
{
    if (!persistence_.load(registry_))
    {
        set_status("No saved configuration could be loaded for " + registry_.appId() + ".");
        return;
    }
    refresh();
    set_status("Reloaded saved configuration for " + registry_.appId() + ".");
}

void ConfigApp::set_status(std::string text)
{
    if (window_ != nullptr)
        window_->set_footer(std::move(text));
}

std::size_t ConfigApp::option_count() const noexcept
{
    return model_ == nullptr ? 0 : model_->row_count();
}

} // namespace ck::vision
