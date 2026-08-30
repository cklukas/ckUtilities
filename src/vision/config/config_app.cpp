#include "config_app.hpp"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/ui/layout.hpp>
#include <cvision/widgets/button.hpp>
#include <cvision/widgets/key_chord_capture.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/static_text.hpp>

namespace ck::vision
{
namespace
{

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
                    option_value_text(definition, registry_.get(definition.key)), std::nullopt, definition.editable};
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

class KeymapTableModel final : public ckv::widgets::TableModel
{
public:
    explicit KeymapTableModel(SuiteKeymapCatalog &catalog) : catalog_(catalog) { refresh(); }

    void refresh() { commands_ = catalog_.commands(); }
    std::size_t row_count() const override { return commands_.size(); }
    ckv::widgets::TableRowId row_id_at(std::size_t index) const override
    {
        return index < commands_.size() ? static_cast<ckv::widgets::TableRowId>(index + 1)
                                        : ckv::widgets::kInvalidTableRowId;
    }
    std::optional<std::size_t> index_of(ckv::widgets::TableRowId id) const override
    {
        if (id == ckv::widgets::kInvalidTableRowId || id > commands_.size())
            return std::nullopt;
        return static_cast<std::size_t>(id - 1);
    }
    ckv::widgets::TableCell cell(ckv::widgets::TableCellRef reference) const override
    {
        const auto index = index_of(reference.row);
        if (!index || reference.column >= 5)
            return {};
        const KeymapCommand &command = commands_[*index];
        switch (reference.column)
        {
        case 0: return {command.application_id, command.application_id, std::nullopt, false};
        case 1: return {command.title, command.title, std::nullopt, false};
        case 2: return {command.key, command.key, std::nullopt, false};
        case 3: return {command.category, command.category, std::nullopt, false};
        case 4:
        {
            const std::string chord = command.active_chord ? ckv::format(*command.active_chord) : "Unbound";
            return {chord, chord, std::nullopt, false};
        }
        }
        return {};
    }
    const KeymapCommand *command(ckv::widgets::TableRowId id) const
    {
        const auto index = index_of(id);
        return index ? &commands_[*index] : nullptr;
    }
    std::optional<ckv::widgets::TableRowId> id_for_command(std::string_view application_id,
                                                           std::string_view key) const
    {
        const auto found = std::find_if(commands_.begin(), commands_.end(), [application_id, key](const KeymapCommand &command) {
            return command.application_id == application_id && command.key == key;
        });
        if (found == commands_.end())
            return std::nullopt;
        return static_cast<ckv::widgets::TableRowId>(std::distance(commands_.begin(), found) + 1);
    }

private:
    SuiteKeymapCatalog &catalog_;
    std::vector<KeymapCommand> commands_;
};

ConfigApp::~ConfigApp() = default;

ConfigApp::ConfigApp(ckv::ui::Application &application,
                     ck::config::OptionRegistry &registry,
                     ConfigPersistence &persistence,
                     KeymapController *keymap)
    : application_(application), registry_(registry), persistence_(persistence), keymap_(keymap),
      model_(std::make_unique<OptionTableModel>(registry_)), command_scope_(application.commands())
{
    declare_commands();
    if (keymap_ != nullptr)
    {
        keymap_catalog_ = std::make_unique<SuiteKeymapCatalog>(*keymap_);
        keymap_scheme_persistence_ = dynamic_cast<KeymapSchemePersistence *>(&keymap_->persistence());
    }
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_window();
}

void ConfigApp::declare_commands()
{
    edit_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-config", "ck.config.edit_selected", [this] { edit_selected(); }));
    reset_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-config", "ck.config.reset_selected", [this] { reset_selected(); }));
    save_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-config", "ck.config.save", [this] { save(); }));
    reload_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-config", "ck.config.reload", [this] { reload(); }));
    import_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-config", "ck.config.import", [this] { show_import_dialog(); }));
    export_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-config", "ck.config.export", [this] { show_export_dialog(); }));
    keymap_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-config", "ck.config.shortcuts", [this] { show_keymap_window(); }));
    keymap_scheme_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-config", "ck.config.shortcuts.scheme", [this] { show_keymap_scheme_dialog(); }));
    const auto selected_option_is_editable = [this] {
        const auto *definition = selected_definition();
        return definition != nullptr && definition->editable;
    };
    application_.commands().set_enabled_predicate(edit_command_, selected_option_is_editable);
    application_.commands().set_enabled_predicate(reset_command_, selected_option_is_editable);
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
                MenuItem::command(CommandPresentation{import_command_, "&Import configuration..."}),
                MenuItem::command(CommandPresentation{export_command_, "&Export configuration..."}),
                MenuItem::command(CommandPresentation{keymap_command_, "Configure &keyboard shortcuts..."}),
                MenuItem::command(CommandPresentation{keymap_scheme_command_, "Select shortcut &scheme..."}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{edit_command_, "&Edit"}, 20},
                StatusLineItem{CommandPresentation{reset_command_, "&Reset"}, 20},
                StatusLineItem{CommandPresentation{save_command_, "&Save"}, 20},
                StatusLineItem{CommandPresentation{reload_command_, "&Reload"}, 25},
                StatusLineItem{CommandPresentation{import_command_, "&Import"}, 20},
                StatusLineItem{CommandPresentation{export_command_, "&Export"}, 20},
                StatusLineItem{CommandPresentation{keymap_command_, "&Shortcuts"}, 24},
                StatusLineItem{CommandPresentation{keymap_scheme_command_, "&Scheme"}, 20},
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

bool ConfigApp::import_configuration(const std::string &path)
{
    if (path.empty())
    {
        set_status("Could not import configuration from " + path + ".");
        return false;
    }

    ck::config::OptionRegistry::Snapshot before_import = registry_.snapshot();
    if (!persistence_.import_from(registry_, path))
    {
        registry_.restore(std::move(before_import));
        set_status("Could not import configuration from " + path + ".");
        return false;
    }
    refresh();
    set_status("Imported configuration from " + path + ".");
    return true;
}

bool ConfigApp::export_configuration(const std::string &path)
{
    if (path.empty() || !persistence_.export_to(registry_, path))
    {
        set_status("Could not export configuration to " + path + ".");
        return false;
    }
    set_status("Exported configuration to " + path + ".");
    return true;
}

const ck::config::OptionDefinition *ConfigApp::selected_definition() const
{
    return selected_key_.empty() ? nullptr : registry_.definition(selected_key_);
}

void ConfigApp::edit_selected()
{
    const auto *definition = selected_definition();
    if (definition == nullptr || !definition->editable)
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
    if (const auto *definition = selected_definition(); definition != nullptr && definition->editable)
    {
        registry_.reset(definition->key);
        refresh();
    }
}

void ConfigApp::save()
{
    const bool configuration_saved = persistence_.save(registry_);
    const bool keymap_saved = keymap_ == nullptr || keymap_->save();
    if (configuration_saved && keymap_saved)
        set_status("Saved configuration for " + registry_.appId() + ".");
    else
        set_status("Could not save all configuration for " + registry_.appId() + ".");
}

void ConfigApp::reload()
{
    const bool configuration_loaded = persistence_.load(registry_);
    const bool keymap_loaded = keymap_catalog_ == nullptr || keymap_catalog_->load();
    if (!configuration_loaded || !keymap_loaded)
    {
        set_status("No saved configuration and shortcuts could be loaded for " + registry_.appId() + ".");
        return;
    }
    refresh();
    refresh_keymap();
    set_status("Reloaded saved configuration for " + registry_.appId() + ".");
}

void ConfigApp::show_import_dialog()
{
    transfer_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Import configuration";
    dialog.fields.push_back({"&Path:", "", [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Import", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    transfer_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    transfer_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && result.values.size() == 1)
            import_configuration(result.values.front());
    });
}

void ConfigApp::show_export_dialog()
{
    transfer_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Export configuration";
    dialog.fields.push_back({"&Path:", registry_.defaultOptionsPath().string() + ".export.json",
                             [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Export", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    transfer_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    transfer_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && result.values.size() == 1)
            export_configuration(result.values.front());
    });
}

void ConfigApp::show_keymap_window()
{
    if (keymap_catalog_ == nullptr)
    {
        set_status("Keyboard shortcut storage is not available in this host.");
        return;
    }
    if (keymap_window_ != nullptr)
    {
        shell_->desktop().activate(keymap_window_);
        return;
    }

    if (!keymap_catalog_->load())
    {
        set_status("Could not load saved keyboard shortcuts.");
        return;
    }
    keymap_model_ = std::make_unique<KeymapTableModel>(*keymap_catalog_);
    auto window = std::make_unique<ckv::widgets::Window>("Keyboard shortcuts: ckUtilities suite");
    window->set_min_size(ckv::Size{88, 18});
    auto content = std::make_unique<ckv::ui::Column>();
    content->set_spacing(1);
    auto table = std::make_unique<ckv::widgets::Table>();
    keymap_table_ = table.get();
    keymap_table_->set_columns({{"Application", 16, 9}, {"Command", 24, 12}, {"Key", 28, 12},
                                {"Category", 16, 8}, {"Shortcut", 18, 9}});
    keymap_table_->set_model(*keymap_model_);
    keymap_table_->on_selection_changed = [this](ckv::widgets::TableCellRef reference) {
        if (const KeymapCommand *command = keymap_model_->command(reference.row))
        {
            selected_command_application_id_ = command->application_id;
            selected_command_key_ = command->key;
        }
    };
    content->add_item(std::move(table), {ckv::ui::SizePolicy::Expanding});

    auto actions = std::make_unique<ckv::ui::Row>();
    actions->set_spacing(2);
    auto edit = std::make_unique<ckv::widgets::Button>("&Edit selected");
    edit->on_press = [this] { edit_selected_shortcut(); };
    actions->add_item(std::move(edit));
    auto reset = std::make_unique<ckv::widgets::Button>("&Reset selected");
    reset->on_press = [this] { reset_selected_shortcut(); };
    actions->add_item(std::move(reset));
    if (keymap_scheme_persistence_ != nullptr)
    {
        auto scheme = std::make_unique<ckv::widgets::Button>("&Scheme...");
        scheme->on_press = [this] { show_keymap_scheme_dialog(); };
        actions->add_item(std::move(scheme));
    }
    auto reload = std::make_unique<ckv::widgets::Button>("&Reload saved");
    reload->on_press = [this] { reload_keymap(); };
    actions->add_item(std::move(reload));
    auto close = std::make_unique<ckv::widgets::Button>("&Close");
    close->on_press = [this] {
        if (keymap_window_ != nullptr)
            keymap_window_->close();
    };
    actions->add_item(std::move(close));
    content->add_item(std::move(actions), {ckv::ui::SizePolicy::Fixed});
    window->set_content(std::move(content));
    ckv::widgets::Window *const raw_window = window.get();
    window->on_closed = [this, raw_window] {
        if (keymap_window_ == raw_window)
        {
            keymap_window_ = nullptr;
            keymap_table_ = nullptr;
            keymap_model_.reset();
            selected_command_application_id_.clear();
            selected_command_key_.clear();
        }
        ckv::widgets::schedule_self_detach(*raw_window, application_);
    };
    keymap_window_ = shell_->desktop().add_window(std::move(window));
    refresh_keymap();
    if (keymap_table_ != nullptr)
        application_.set_focus(keymap_table_);
}

void ConfigApp::show_keymap_scheme_dialog()
{
    if (keymap_scheme_persistence_ == nullptr)
    {
        set_keymap_status("Shortcut schemes are not available in this host.");
        return;
    }
    const std::vector<KeymapScheme> schemes = keymap_scheme_persistence_->keymap_schemes();
    if (schemes.empty())
    {
        set_keymap_status("No shortcut schemes are available.");
        return;
    }
    std::vector<std::string> titles;
    titles.reserve(schemes.size());
    std::size_t selected = 0;
    const std::string active = keymap_scheme_persistence_->active_keymap_scheme();
    for (std::size_t index = 0; index < schemes.size(); ++index)
    {
        titles.push_back(schemes[index].title);
        if (schemes[index].id == active)
            selected = index;
    }

    keymap_scheme_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Select shortcut scheme";
    dialog.fields.push_back({"&Scheme", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             std::move(titles), static_cast<int>(selected)});
    dialog.buttons.push_back({"&Use scheme", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    keymap_scheme_dialog_.emplace(
        ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    keymap_scheme_dialog_->set_completion_handler([this, schemes](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.empty() || result.selected.front() >= schemes.size())
            return;
        select_keymap_scheme(schemes[result.selected.front()].id);
    });
}

void ConfigApp::edit_selected_shortcut()
{
    KeymapController *const controller = selected_keymap_controller();
    if (controller == nullptr || keymap_model_ == nullptr || selected_command_key_.empty() || shortcut_window_ != nullptr)
        return;
    const std::vector<KeymapCommand> commands = controller->commands();
    const auto selected = std::find_if(commands.begin(), commands.end(), [this](const KeymapCommand &command) {
        return command.application_id == selected_command_application_id_ && command.key == selected_command_key_;
    });
    if (selected == commands.end())
        return;

    auto window = std::make_unique<ckv::widgets::Window>("Shortcut: " + selected->title);
    window->set_min_size(ckv::Size{52, 10});
    auto content = std::make_unique<ckv::ui::Column>();
    content->set_spacing(1);
    content->add_item(std::make_unique<ckv::widgets::StaticText>(
        "Press Enter or Space, then press the shortcut. Escape cancels capture; Backspace clears."));
    auto capture = std::make_unique<ckv::widgets::KeyChordCapture>();
    ckv::widgets::KeyChordCapture *const capture_view = capture.get();
    capture_view->set_chord(selected->active_chord);
    content->add_item(std::move(capture));
    auto actions = std::make_unique<ckv::ui::Row>();
    actions->set_spacing(2);
    const std::string command_key = selected->key;
    auto apply = std::make_unique<ckv::widgets::Button>("&Apply");
    apply->on_press = [this, controller, command_key, capture_view] {
        const std::optional<ckv::KeyChord> requested = capture_view->chord();
        const KeymapUpdate update = controller->update(command_key, requested);
        if (update.status == KeymapUpdateStatus::Applied)
        {
            save_keymap(*controller);
            refresh_keymap();
            if (shortcut_window_ != nullptr)
                shortcut_window_->close();
            return;
        }
        if (update.status != KeymapUpdateStatus::Conflict || !update.conflict)
        {
            set_keymap_status("The selected command is no longer available.");
            return;
        }
        const std::string occupied = update.conflict->existing_command_key;
        keymap_conflict_ = ckv::widgets::present_message_box(
            application_, shell_->desktop(), shell_->roles(),
            {ckv::widgets::MessageBoxKind::Confirm, "Replace shortcut?",
             ckv::format(*requested) + " is assigned to " + occupied + ". Replace it?",
             ckv::widgets::MessageBoxButtons::YesNo});
        keymap_conflict_->set_completion_handler([this, controller, command_key, requested](ckv::widgets::MessageBoxResult result) {
            if (result != ckv::widgets::MessageBoxResult::Yes)
                return;
            if (controller->update(command_key, requested, true).status == KeymapUpdateStatus::Applied)
            {
                save_keymap(*controller);
                refresh_keymap();
                if (shortcut_window_ != nullptr)
                    shortcut_window_->close();
            }
        });
    };
    actions->add_item(std::move(apply));
    auto cancel = std::make_unique<ckv::widgets::Button>("&Cancel");
    cancel->on_press = [this] {
        if (shortcut_window_ != nullptr)
            shortcut_window_->close();
    };
    actions->add_item(std::move(cancel));
    content->add_item(std::move(actions), {ckv::ui::SizePolicy::Fixed});
    window->set_content(std::move(content));
    ckv::widgets::Window *const raw_window = window.get();
    window->on_closed = [this, raw_window] {
        if (shortcut_window_ == raw_window)
            shortcut_window_ = nullptr;
        ckv::widgets::schedule_self_detach(*raw_window, application_);
    };
    shortcut_window_ = shell_->desktop().add_window(std::move(window));
    application_.set_focus(capture_view);
}

void ConfigApp::reset_selected_shortcut()
{
    KeymapController *const controller = selected_keymap_controller();
    if (controller == nullptr || selected_command_key_.empty() || !controller->reset(selected_command_key_))
        return;
    save_keymap(*controller);
    refresh_keymap();
}

void ConfigApp::reload_keymap()
{
    if (keymap_catalog_ != nullptr && keymap_catalog_->load())
    {
        refresh_keymap();
        set_keymap_status("Reloaded saved keyboard shortcuts.");
        return;
    }
    set_keymap_status("Could not load saved keyboard shortcuts.");
}

void ConfigApp::refresh_keymap()
{
    if (keymap_model_ == nullptr || keymap_table_ == nullptr)
        return;
    keymap_model_->refresh();
    keymap_table_->model_changed();
    if (selected_command_key_.empty() && keymap_model_->row_count() != 0)
    {
        const KeymapCommand *const first = keymap_model_->command(1);
        selected_command_application_id_ = first->application_id;
        selected_command_key_ = first->key;
    }
    if (const auto id = keymap_model_->id_for_command(selected_command_application_id_, selected_command_key_))
        keymap_table_->set_selected_cell({*id, 0});
    std::string status = std::to_string(keymap_model_->row_count()) + " suite commands; Enter/Space captures a shortcut.";
    if (keymap_scheme_persistence_ != nullptr)
        status += " Active scheme: " + keymap_scheme_persistence_->active_keymap_scheme() + ".";
    set_keymap_status(std::move(status));
}

void ConfigApp::save_keymap(KeymapController &controller)
{
    if (controller.save())
        set_keymap_status("Saved keyboard shortcuts.");
    else
        set_keymap_status("Could not save keyboard shortcuts.");
}

void ConfigApp::set_status(std::string text)
{
    if (window_ != nullptr)
        window_->set_footer(std::move(text));
}

void ConfigApp::set_keymap_status(std::string text)
{
    if (keymap_window_ != nullptr)
        keymap_window_->set_footer(std::move(text));
}

std::size_t ConfigApp::option_count() const noexcept
{
    return model_ == nullptr ? 0 : model_->row_count();
}

std::size_t ConfigApp::keymap_command_count() const noexcept
{
    return keymap_catalog_ == nullptr ? 0 : keymap_catalog_->commands().size();
}

bool ConfigApp::select_keymap_command(std::string_view application_id, std::string_view command_key)
{
    if (keymap_model_ == nullptr || keymap_table_ == nullptr)
        return false;
    const auto id = keymap_model_->id_for_command(application_id, command_key);
    if (!id)
        return false;
    selected_command_application_id_ = application_id;
    selected_command_key_ = command_key;
    keymap_table_->set_selected_cell({*id, 0});
    application_.set_focus(keymap_table_);
    return true;
}

bool ConfigApp::select_keymap_scheme(std::string_view scheme_id)
{
    if (keymap_scheme_persistence_ == nullptr || keymap_catalog_ == nullptr)
    {
        set_keymap_status("Could not select shortcut scheme.");
        return false;
    }

    const std::string previous_scheme = keymap_scheme_persistence_->active_keymap_scheme();
    if (!keymap_scheme_persistence_->select_keymap_scheme(scheme_id))
    {
        set_keymap_status("Could not select shortcut scheme.");
        return false;
    }
    if (!keymap_catalog_->load())
    {
        // A scheme switch is not useful unless its bindings can become the
        // active view. Restore the persisted selection and, when possible,
        // restore the command registries to that prior scheme as well.
        if (previous_scheme != scheme_id &&
            keymap_scheme_persistence_->select_keymap_scheme(previous_scheme))
            keymap_catalog_->load();
        set_keymap_status("Could not select shortcut scheme.");
        return false;
    }
    refresh_keymap();
    return true;
}

KeymapController *ConfigApp::selected_keymap_controller() const noexcept
{
    if (keymap_catalog_ == nullptr || selected_command_application_id_.empty())
        return nullptr;
    return keymap_catalog_->controller_for(selected_command_application_id_);
}

} // namespace ck::vision
