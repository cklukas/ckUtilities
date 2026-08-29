#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/options.hpp"
#include "ck/vision/keymap.hpp"
#include "ck/vision/suite_shell.hpp"
#include "config_persistence.hpp"

namespace ck::vision
{

class OptionTableModel;

// Native registry inspector/editor. Persistence remains an injected
// composition-root policy rather than a view filesystem responsibility.
class ConfigApp
{
public:
    ConfigApp(ckv::ui::Application &application,
              ck::config::OptionRegistry &registry,
              ConfigPersistence &persistence,
              KeymapController *keymap = nullptr);
    ~ConfigApp();

    bool select_option(std::string_view key);
    bool import_configuration(const std::string &path);
    bool export_configuration(const std::string &path);
    std::string selected_key() const { return selected_key_; }
    std::size_t option_count() const noexcept;
    ckv::widgets::Table *table() const noexcept { return table_; }
    ckv::ui::CommandId edit_command() const noexcept { return edit_command_; }
    ckv::ui::CommandId reset_command() const noexcept { return reset_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId reload_command() const noexcept { return reload_command_; }
    ckv::ui::CommandId import_command() const noexcept { return import_command_; }
    ckv::ui::CommandId export_command() const noexcept { return export_command_; }
    ckv::ui::CommandId keymap_command() const noexcept { return keymap_command_; }
    std::size_t keymap_command_count() const noexcept;

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_window();
    void refresh();
    void edit_selected();
    void reset_selected();
    void save();
    void reload();
    void show_import_dialog();
    void show_export_dialog();
    void show_keymap_window();
    void edit_selected_shortcut();
    void reset_selected_shortcut();
    void reload_keymap();
    void refresh_keymap();
    void save_keymap();
    void set_status(std::string text);
    void set_keymap_status(std::string text);
    const ck::config::OptionDefinition *selected_definition() const;

    ckv::ui::Application &application_;
    ck::config::OptionRegistry &registry_;
    ConfigPersistence &persistence_;
    KeymapController *keymap_ = nullptr;
    std::unique_ptr<OptionTableModel> model_;
    std::unique_ptr<class KeymapTableModel> keymap_model_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::Table *table_ = nullptr;
    ckv::widgets::Window *keymap_window_ = nullptr;
    ckv::widgets::Window *shortcut_window_ = nullptr;
    ckv::widgets::Table *keymap_table_ = nullptr;
    ckv::ui::CommandId edit_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId reset_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId reload_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId import_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId export_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId keymap_command_ = ckv::ui::kInvalidCommand;
    std::string selected_key_;
    std::string selected_command_key_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> edit_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> transfer_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> keymap_conflict_;
};

} // namespace ck::vision
