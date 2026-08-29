#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/options.hpp"
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
              ConfigPersistence &persistence);
    ~ConfigApp();

    bool select_option(std::string_view key);
    std::string selected_key() const { return selected_key_; }
    std::size_t option_count() const noexcept;
    ckv::widgets::Table *table() const noexcept { return table_; }
    ckv::ui::CommandId edit_command() const noexcept { return edit_command_; }
    ckv::ui::CommandId reset_command() const noexcept { return reset_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId reload_command() const noexcept { return reload_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_window();
    void refresh();
    void edit_selected();
    void reset_selected();
    void save();
    void reload();
    void set_status(std::string text);
    const ck::config::OptionDefinition *selected_definition() const;

    ckv::ui::Application &application_;
    ck::config::OptionRegistry &registry_;
    ConfigPersistence &persistence_;
    std::unique_ptr<OptionTableModel> model_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::Table *table_ = nullptr;
    ckv::ui::CommandId edit_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId reset_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId reload_command_ = ckv::ui::kInvalidCommand;
    std::string selected_key_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> edit_dialog_;
};

} // namespace ck::vision
