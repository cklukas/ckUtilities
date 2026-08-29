#pragma once

#include <memory>
#include <optional>
#include <string>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/text_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/find/search_model.hpp"
#include "ck/vision/suite_shell.hpp"

namespace ck::vision
{

class FindApp
{
public:
    explicit FindApp(ckv::ui::Application &application);

    const ck::find::SearchSpecification &specification() const noexcept { return specification_; }
    std::string command_preview() const;
    ckv::ui::CommandId new_search_command() const noexcept { return new_search_command_; }
    ckv::ui::CommandId preview_command() const noexcept { return preview_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void show_guided_search_dialog();
    void show_preview();
    void present_text_window(std::string title, std::string content);

    ckv::ui::Application &application_;
    ck::find::SearchSpecification specification_;
    ckv::ui::CommandId new_search_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_command_ = ckv::ui::kInvalidCommand;
    std::unique_ptr<SuiteShell> shell_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> search_dialog_;
};

} // namespace ck::vision
