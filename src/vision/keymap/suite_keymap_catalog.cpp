#include "ck/vision/keymap.hpp"

#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace ck::vision
{
namespace
{

using CommandDefinition = SuiteCommandMetadata;

template <std::size_t Count>
void declare_commands(ckv::ui::CommandRegistry &registry, const std::array<CommandDefinition, Count> &definitions)
{
    for (const CommandDefinition &definition : definitions)
    {
        registry.declare({.key = std::string(definition.key),
                          .title = std::string(definition.title),
                          .category = std::string(definition.category),
                          .chord = std::string(definition.default_chord),
                          .visibility = ckv::ui::CommandVisibility::Palette});
    }
}

const std::array kJsonViewCommands{
    CommandDefinition{"ck.json_view.open", "&Open JSON...", "JSON View", "Ctrl+O"},
    CommandDefinition{"ck.json_view.reload", "&Reload JSON", "JSON View", "Ctrl+R"},
    CommandDefinition{"ck.json_view.close", "&Close JSON", "JSON View", "Ctrl+W"},
    CommandDefinition{"ck.json_view.copy", "&Copy selected JSON", "JSON View", "Ctrl+C"},
    CommandDefinition{"ck.json_view.find", "&Find...", "JSON View", "Ctrl+F"},
    CommandDefinition{"ck.json_view.find_next", "Find &Next", "JSON View", "F3"},
    CommandDefinition{"ck.json_view.find_previous", "Find &Previous", "JSON View", "Shift+F3"},
    CommandDefinition{"ck.json_view.end_search", "&End search", "JSON View", "Esc"},
    CommandDefinition{"ck.json_view.expand_level.0", "Expand to level &0", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.1", "Expand to level &1", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.2", "Expand to level &2", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.3", "Expand to level &3", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.4", "Expand to level &4", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.5", "Expand to level &5", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.6", "Expand to level &6", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.7", "Expand to level &7", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.8", "Expand to level &8", "JSON View", ""},
    CommandDefinition{"ck.json_view.expand_level.9", "Expand to level &9", "JSON View", ""},
};

const std::array kLauncherCommands{
    CommandDefinition{"ck.utilities.launch_tool", "&Launch selected tool", "CK Utilities", "Enter"},
    CommandDefinition{"ck.utilities.new_launcher", "&New launcher window", "CK Utilities", "Ctrl+N"},
    CommandDefinition{"ck.utilities.show_calendar", "Show &Calendar", "CK Utilities", ""},
    CommandDefinition{"ck.utilities.show_ascii_table", "Show &ASCII table", "CK Utilities", ""},
    CommandDefinition{"ck.utilities.show_calculator", "Show &Calculator", "CK Utilities", ""},
    CommandDefinition{"ck.utilities.show_diagnostics", "Show &Diagnostics", "CK Utilities", ""},
    CommandDefinition{"ck.utilities.show_color_selector", "Show color &selector", "CK Utilities", ""},
};

const std::array kFindCommands{
    CommandDefinition{"ck.find.new_search", "&New search...", "Find", "Ctrl+N"},
    CommandDefinition{"ck.find.preview_command", "&Preview command", "Find", "F5"},
    CommandDefinition{"ck.find.save_search", "&Save search...", "Find", "Ctrl+S"},
    CommandDefinition{"ck.find.load_search", "&Load saved search...", "Find", "Ctrl+O"},
    CommandDefinition{"ck.find.execute_search", "&Run search", "Find", "F9"},
    CommandDefinition{"ck.find.cancel_search", "&Cancel search", "Find", "Ctrl+C"},
};

const std::array kDiskUsageCommands{
    CommandDefinition{"ck.du.rescan", "&Rescan", "Disk usage", "F5"},
    CommandDefinition{"ck.du.cancel_scan", "&Cancel scan", "Disk usage", "Ctrl+C"},
    CommandDefinition{"ck.du.view_files", "&View files", "Disk usage", "Enter"},
    CommandDefinition{"ck.du.cloud.download", "&Download selected", "Cloud storage", "Ctrl+D"},
    CommandDefinition{"ck.du.cloud.evict", "&Free local copies", "Cloud storage", "Ctrl+E"},
    CommandDefinition{"ck.du.cloud.cancel", "Cancel &cloud operation", "Cloud storage", "Ctrl+Shift+C"},
};

const std::array kEditCommands{
    CommandDefinition{"ck.edit.open", "&Open document...", "Editor", "Ctrl+O"},
    CommandDefinition{"ck.edit.save", "&Save", "Editor", "Ctrl+S"},
    CommandDefinition{"ck.edit.save_as", "Save &As...", "Editor", ""},
    CommandDefinition{"ck.edit.normalise_markdown", "&Normalize Markdown whitespace", "Editor", ""},
    CommandDefinition{"ck.edit.undo", "&Undo", "Editor", "Ctrl+Z"},
    CommandDefinition{"ck.edit.redo", "&Redo", "Editor", "Ctrl+Y"},
    CommandDefinition{"ck.edit.toggle_wrap", "Toggle word &wrap", "Editor", "Alt+W"},
    CommandDefinition{"ck.edit.find", "&Find...", "Search", "Ctrl+F"},
    CommandDefinition{"ck.edit.find_next", "Find &next", "Search", "F3"},
    CommandDefinition{"ck.edit.replace", "&Replace...", "Search", "Ctrl+H"},
    CommandDefinition{"ck.edit.replace_all", "Replace &all...", "Search", ""},
    CommandDefinition{"ck.edit.reflow_markdown", "&Reflow Markdown paragraph", "Markdown format", ""},
    CommandDefinition{"ck.edit.bold", "&Bold selection", "Markdown format", "Ctrl+B"},
    CommandDefinition{"ck.edit.italic", "&Italic selection", "Markdown format", "Ctrl+I"},
    CommandDefinition{"ck.edit.strikethrough", "&Strikethrough selection", "Markdown format", ""},
    CommandDefinition{"ck.edit.inline_code", "Inline &code selection", "Markdown format", ""},
    CommandDefinition{"ck.edit.toggle_link", "Insert or remove &link...", "Markdown format", ""},
    CommandDefinition{"ck.edit.toggle_image", "Insert or remove &image...", "Markdown format", ""},
    CommandDefinition{"ck.edit.insert_footnote", "Insert &footnote...", "Markdown format", ""},
    CommandDefinition{"ck.edit.insert_table", "Insert &table...", "Markdown format", ""},
    CommandDefinition{"ck.edit.insert_table_row", "Add table &row", "Markdown format", ""},
    CommandDefinition{"ck.edit.erase_table_row", "Delete table row", "Markdown format", ""},
    CommandDefinition{"ck.edit.insert_table_column", "Add table &column", "Markdown format", ""},
    CommandDefinition{"ck.edit.erase_table_column", "Delete table column", "Markdown format", ""},
    CommandDefinition{"ck.edit.toggle_task", "Toggle &task", "Markdown format", ""},
    CommandDefinition{"ck.edit.toggle_quote", "Toggle &quote", "Markdown format", ""},
    CommandDefinition{"ck.edit.toggle_bullet_list", "Toggle &bullet list", "Markdown format", ""},
    CommandDefinition{"ck.edit.toggle_ordered_list", "Toggle &ordered list", "Markdown format", ""},
    CommandDefinition{"ck.edit.indent_list", "&Indent list", "Markdown format", ""},
    CommandDefinition{"ck.edit.outdent_list", "&Outdent list", "Markdown format", ""},
    CommandDefinition{"ck.edit.heading.1", "Heading &1", "Markdown format", ""},
    CommandDefinition{"ck.edit.heading.2", "Heading &2", "Markdown format", ""},
    CommandDefinition{"ck.edit.heading.3", "Heading &3", "Markdown format", ""},
    CommandDefinition{"ck.edit.heading.4", "Heading &4", "Markdown format", ""},
    CommandDefinition{"ck.edit.heading.5", "Heading &5", "Markdown format", ""},
    CommandDefinition{"ck.edit.heading.6", "Heading &6", "Markdown format", ""},
};

const std::array kChatCommands{
    CommandDefinition{"ck.chat.new_chat", "&New conversation", "Chat", "Ctrl+N"},
    CommandDefinition{"ck.chat.send_prompt", "&Send prompt...", "Chat", "Ctrl+Enter"},
    CommandDefinition{"ck.chat.cancel_response", "&Cancel response", "Chat", "Ctrl+C"},
    CommandDefinition{"ck.chat.copy_transcript", "&Copy transcript", "Chat", ""},
    CommandDefinition{"ck.chat.export_transcript", "&Export transcript...", "Chat", ""},
    CommandDefinition{"ck.chat.select_prompt", "Select system &prompt...", "Chat", ""},
    CommandDefinition{"ck.chat.add_prompt", "&Add system prompt...", "Chat", ""},
    CommandDefinition{"ck.chat.edit_active_prompt", "&Edit active prompt...", "Chat", ""},
    CommandDefinition{"ck.chat.restore_active_prompt", "&Restore active default prompt", "Chat", ""},
    CommandDefinition{"ck.chat.delete_active_prompt", "&Delete active prompt", "Chat", ""},
    CommandDefinition{"ck.chat.select_model", "Select active &model...", "Chat", ""},
    CommandDefinition{"ck.chat.download_model", "&Download model...", "Chat", ""},
    CommandDefinition{"ck.chat.cancel_model_download", "Cancel model &download", "Chat", ""},
    CommandDefinition{"ck.chat.deactivate_model", "&Deactivate active model", "Chat", ""},
    CommandDefinition{"ck.chat.delete_active_model", "&Delete active model", "Chat", ""},
};

const std::array kConfigCommands{
    CommandDefinition{"ck.config.select_application", "Select &application...", "Configuration", ""},
    CommandDefinition{"ck.config.edit_selected", "&Edit selected option", "Configuration", "Enter"},
    CommandDefinition{"ck.config.reset_selected", "&Reset selected option", "Configuration", ""},
    CommandDefinition{"ck.config.save", "&Save configuration", "Configuration", "Ctrl+S"},
    CommandDefinition{"ck.config.reload", "&Reload saved configuration", "Configuration", "Ctrl+R"},
    CommandDefinition{"ck.config.import", "&Import configuration...", "Configuration", ""},
    CommandDefinition{"ck.config.export", "&Export configuration...", "Configuration", ""},
    CommandDefinition{"ck.config.shortcuts", "Configure &keyboard shortcuts...", "Configuration", ""},
    CommandDefinition{"ck.config.shortcuts.scheme", "Select shortcut &scheme...", "Configuration", ""},
};

template <std::size_t Count>
const SuiteCommandMetadata *find_in(const std::array<CommandDefinition, Count> &definitions,
                                    std::string_view command_key) noexcept
{
    for (const CommandDefinition &definition : definitions)
    {
        if (definition.key == command_key)
            return &definition;
    }
    return nullptr;
}

} // namespace

const SuiteCommandMetadata *find_suite_command(std::string_view application_id,
                                                std::string_view command_key) noexcept
{
    if (application_id == "ck-json-view") return find_in(kJsonViewCommands, command_key);
    if (application_id == "ck-utilities") return find_in(kLauncherCommands, command_key);
    if (application_id == "ck-find") return find_in(kFindCommands, command_key);
    if (application_id == "ck-du") return find_in(kDiskUsageCommands, command_key);
    if (application_id == "ck-config") return find_in(kConfigCommands, command_key);
    if (application_id == "ck-edit") return find_in(kEditCommands, command_key);
    if (application_id == "ck-chat") return find_in(kChatCommands, command_key);
    return nullptr;
}

ckv::ui::CommandId declare_suite_command(ckv::ui::CommandRegistry &registry,
                                         std::string_view application_id,
                                         std::string_view command_key,
                                         std::function<void()> handler)
{
    const SuiteCommandMetadata *const metadata = find_suite_command(application_id, command_key);
    if (metadata == nullptr)
        return ckv::ui::kInvalidCommand;
    return registry.declare({.key = std::string(metadata->key),
                             .title = std::string(metadata->title),
                             .category = std::string(metadata->category),
                             .chord = std::string(metadata->default_chord),
                             .visibility = ckv::ui::CommandVisibility::Palette,
                             .handler = std::move(handler)});
}

struct SuiteKeymapCatalog::Entry
{
    std::string application_id;
    std::unique_ptr<ckv::ui::CommandRegistry> registry;
    std::unique_ptr<KeymapController> controller;
};

SuiteKeymapCatalog::SuiteKeymapCatalog(KeymapController &active_controller)
    : active_controller_(&active_controller)
{
    const auto add = [this, &active_controller](std::string_view application_id, const auto &definitions) {
        auto entry = std::make_unique<Entry>();
        entry->application_id = application_id;
        entry->registry = std::make_unique<ckv::ui::CommandRegistry>();
        declare_commands(*entry->registry, definitions);
        entry->controller = std::make_unique<KeymapController>(entry->application_id, *entry->registry,
                                                                active_controller.persistence());
        entries_.push_back(std::move(entry));
    };

    add("ck-json-view", kJsonViewCommands);
    add("ck-utilities", kLauncherCommands);
    add("ck-find", kFindCommands);
    add("ck-du", kDiskUsageCommands);
    add("ck-edit", kEditCommands);
    add("ck-chat", kChatCommands);
}

SuiteKeymapCatalog::~SuiteKeymapCatalog() = default;

std::vector<KeymapCommand> SuiteKeymapCatalog::commands() const
{
    std::vector<KeymapCommand> result = active_controller_->commands();
    for (const auto &entry : entries_)
    {
        for (KeymapCommand command : entry->controller->commands())
        {
            if (!command.key.starts_with("ckv."))
                result.push_back(std::move(command));
        }
    }
    return result;
}

KeymapController *SuiteKeymapCatalog::controller_for(std::string_view application_id) noexcept
{
    if (active_controller_->application_id() == application_id)
        return active_controller_;
    for (const auto &entry : entries_)
    {
        if (entry->application_id == application_id)
            return entry->controller.get();
    }
    return nullptr;
}

bool SuiteKeymapCatalog::load()
{
    bool loaded = active_controller_->load();
    for (const auto &entry : entries_)
        loaded = entry->controller->load() && loaded;
    return loaded;
}

} // namespace ck::vision
