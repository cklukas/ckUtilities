#pragma once

#include <memory>
#include <optional>
#include <string>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/text_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/find/search_model.hpp"
#include "ck/vision/suite_shell.hpp"
#include "find_services.hpp"

namespace ck::vision
{

class FindApp
{
public:
    FindApp(ckv::ui::Application &application,
            FindSpecificationStore &specification_store,
            FindExecutionService &execution_service);
    ~FindApp();

    const ck::find::SearchSpecification &specification() const noexcept { return specification_; }
    void set_specification(ck::find::SearchSpecification specification);
    std::string command_preview() const;
    ckv::ui::CommandId new_search_command() const noexcept { return new_search_command_; }
    ckv::ui::CommandId preview_command() const noexcept { return preview_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId load_command() const noexcept { return load_command_; }
    ckv::ui::CommandId execute_command() const noexcept { return execute_command_; }
    ckv::ui::CommandId cancel_command() const noexcept { return cancel_command_; }
    bool execution_running() const noexcept;
    const std::optional<ck::find::SearchExecutionResult> &last_execution_result() const noexcept
    {
        return last_execution_result_;
    }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void show_guided_search_dialog();
    void show_save_dialog();
    void show_load_dialog();
    void show_preview();
    void request_execution();
    void start_execution(bool delete_matched_files);
    void cancel_execution();
    void complete_execution(ck::find::SearchExecutionResult result);
    void present_text_window(std::string title, std::string content);

    ckv::ui::Application &application_;
    FindSpecificationStore &specification_store_;
    FindExecutionService &execution_service_;
    ck::find::SearchSpecification specification_;
    ckv::ui::CommandId new_search_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId load_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId execute_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_command_ = ckv::ui::kInvalidCommand;
    std::unique_ptr<SuiteShell> shell_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> search_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> destructive_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> custom_command_confirmation_;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::optional<ck::find::SearchExecutionResult> last_execution_result_;
    SuiteCommandScope command_scope_;
};

} // namespace ck::vision
