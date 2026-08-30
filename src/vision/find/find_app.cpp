#include "find_app.hpp"

#include <sstream>
#include <utility>
#include <vector>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/text_layout.hpp>

#include "ck/vision/keymap.hpp"
#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/guided_search.hpp"
#include "ck/find/search_backend.hpp"

namespace ck::vision
{
namespace
{
using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;
}

FindApp::FindApp(ckv::ui::Application &application,
                 FindSpecificationStore &specification_store,
                 FindExecutionService &execution_service)
    : application_(application),
      specification_store_(specification_store),
      execution_service_(execution_service),
      specification_(ck::find::makeDefaultSpecification()),
      command_scope_(application.commands())
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    show_preview();
}

FindApp::~FindApp()
{
    // Completion callbacks are posted from a worker.  Expire their gate
    // before requesting cancellation so an already queued callback cannot
    // access a presentation tree being torn down.
    lifetime_.reset();
    execution_service_.cancel();
}

void FindApp::declare_commands()
{
    new_search_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.new_search", [this] { show_guided_search_dialog(); }));
    preview_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.preview_command", [this] { show_preview(); }));
    save_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.save_search", [this] { show_save_dialog(); }));
    load_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.load_search", [this] { show_load_dialog(); }));
    execute_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.execute_search", [this] { request_execution(); }));
    cancel_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.cancel_search", [this] { cancel_execution(); }));
}

SuiteShellOptions FindApp::make_shell_options() const
{
    return {.application_name = "ck Find",
            .about_text = "A native ckVision front end for saving, previewing, and safely running reusable file-search specifications.",
            .application_menus = {MenuBarItem{"&Search", {
                MenuItem::command(CommandPresentation{new_search_command_, "&New search..."}),
                MenuItem::command(CommandPresentation{save_command_, "&Save search..."}),
                MenuItem::command(CommandPresentation{load_command_, "&Load saved search..."}),
                MenuItem::command(CommandPresentation{preview_command_, "&Preview command"}),
                MenuItem::command(CommandPresentation{execute_command_, "&Run search"}),
                MenuItem::command(CommandPresentation{cancel_command_, "&Cancel search"}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{new_search_command_, "&New"}, 30},
                StatusLineItem{CommandPresentation{save_command_, "&Save"}, 30},
                StatusLineItem{CommandPresentation{load_command_, "&Load"}, 30},
                StatusLineItem{CommandPresentation{preview_command_, "&Preview"}, 25},
                StatusLineItem{CommandPresentation{execute_command_, "&Run"}, 25},
                StatusLineItem{CommandPresentation{cancel_command_, "&Cancel"}, 30},
            }};
}

void FindApp::show_guided_search_dialog()
{
    search_dialog_.reset();
    const ck::find::GuidedSearchState state = ck::find::guidedStateFromSpecification(specification_);
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Guided search";
    dialog.resizable = true;
    dialog.minimum_window_size = ckv::Size{64, 20};
    dialog.fields.push_back({"&Search name:", ck::find::bufferToString(state.specName), nullptr});
    dialog.fields.push_back({"&Start location:", ck::find::bufferToString(state.startLocation),
                             [](const std::string &value) { return !value.empty(); }});
    dialog.fields.push_back({"Search &text:", ck::find::bufferToString(state.searchText), nullptr});
    dialog.fields.push_back({"Include &patterns:", ck::find::bufferToString(state.includePatterns), nullptr});
    dialog.fields.push_back({"E&xclude patterns:", ck::find::bufferToString(state.excludePatterns), nullptr});
    dialog.fields.push_back({"Search &subdirectories", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.includeSubdirectories});
    dialog.fields.push_back({"Include &hidden files", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.includeHidden});
    dialog.fields.push_back({"Follow symbolic &links", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.followSymlinks});
    dialog.fields.push_back({"Stay on the same &filesystem", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.stayOnSameFilesystem});
    dialog.fields.push_back({"Search &target", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             {"Contents and file names", "Contents only", "File names only"},
                             state.searchFileContents && state.searchFileNames ? 0 : state.searchFileContents ? 1 : 2});
    dialog.fields.push_back({"Text &matching", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             {"Contains", "Whole word", "Regular expression"}, static_cast<int>(state.textMode)});
    dialog.fields.push_back({"Match &case", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.textMatchCase});
    dialog.fields.push_back({"Allow &multiple terms", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.textAllowMultipleTerms});
    dialog.fields.push_back({"Treat &binary files as text", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.textTreatBinaryAsText});
    dialog.fields.push_back({"File &type", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             {"All", "Documents", "Images", "Audio", "Archives", "Code", "Custom"},
                             static_cast<int>(state.typePreset)});
    dialog.fields.push_back({"Custom e&xtensions:", ck::find::bufferToString(state.typeCustomExtensions), nullptr});
    dialog.fields.push_back({"Date &range", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             {"Any time", "Past day", "Past week", "Past month", "Past six months", "Past year", "Custom range"},
                             static_cast<int>(state.datePreset)});
    dialog.fields.push_back({"Date &from:", ck::find::bufferToString(state.dateFrom), nullptr});
    dialog.fields.push_back({"Date &to:", ck::find::bufferToString(state.dateTo), nullptr});
    dialog.fields.push_back({"&Size", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             {"Any size", "Larger than", "Smaller than", "Between", "Exactly", "Empty only"},
                             static_cast<int>(state.sizePreset)});
    dialog.fields.push_back({"Primary size:", ck::find::bufferToString(state.sizePrimary), nullptr});
    dialog.fields.push_back({"Secondary size:", ck::find::bufferToString(state.sizeSecondary), nullptr});
    dialog.fields.push_back({"Use &decimal size units", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.sizeUseDecimalUnits});
    dialog.fields.push_back({"Include permission &audit", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.includePermissionAudit});
    dialog.fields.push_back({"Fine-tune &traversal", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.includeTraversalFineTune});
    dialog.fields.push_back({"Enable &actions", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.includeActionTweaks});
    dialog.fields.push_back({"&List matches", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.listMatches});
    dialog.fields.push_back({"&Delete matches", "", nullptr, false, '*', ckv::widgets::FieldKind::Check,
                             state.deleteMatches});
    const std::string existing_custom_command = ck::find::bufferToString(specification_.actionOptions.execCommand);
    const int custom_command_template = specification_.enableActionOptions && specification_.actionOptions.execEnabled
                                            ? existing_custom_command == "ckvision.file-info" ? 1
                                              : existing_custom_command == "ckvision.sha256" ? 2
                                                                                               : 0
                                            : 0;
    dialog.fields.push_back({"Sandboxed custom &action", "", nullptr, false, '*', ckv::widgets::FieldKind::Radio, false,
                             {"Disabled (retain saved setting)", "File metadata", "SHA-256 digest"}, custom_command_template});
    dialog.fields.push_back({.label = "Only fixed, read-only ckvision templates can run. They use direct argv in the macOS sandbox; any legacy custom value stays saved but is refused.",
                             .kind = ckv::widgets::FieldKind::Note});
    dialog.buttons.push_back({"&Apply", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    search_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    search_dialog_->set_completion_handler([this, existing_custom_command](ckv::widgets::DialogResult result) {
        constexpr std::size_t kFieldCount = 30;
        if (!result.accepted || result.values.size() != kFieldCount || result.checked.size() != kFieldCount ||
            result.selected.size() != kFieldCount || result.selected[9] < 0 || result.selected[10] < 0 ||
            result.selected[14] < 0 || result.selected[16] < 0 || result.selected[19] < 0 || result.selected[28] < 0)
            return;

        ck::find::GuidedSearchState applied = ck::find::guidedStateFromSpecification(specification_);
        ck::find::copyToArray(applied.specName, result.values[0].c_str());
        ck::find::copyToArray(applied.startLocation, result.values[1].c_str());
        ck::find::copyToArray(applied.searchText, result.values[2].c_str());
        ck::find::copyToArray(applied.includePatterns, result.values[3].c_str());
        ck::find::copyToArray(applied.excludePatterns, result.values[4].c_str());
        applied.includeSubdirectories = result.checked[5];
        applied.includeHidden = result.checked[6];
        applied.followSymlinks = result.checked[7];
        applied.stayOnSameFilesystem = result.checked[8];
        applied.searchFileContents = result.selected[9] != 2;
        applied.searchFileNames = result.selected[9] != 1;
        applied.textMode = static_cast<ck::find::TextSearchOptions::Mode>(result.selected[10]);
        applied.textMatchCase = result.checked[11];
        applied.textAllowMultipleTerms = result.checked[12];
        applied.textTreatBinaryAsText = result.checked[13];
        applied.typePreset = static_cast<ck::find::GuidedTypePreset>(result.selected[14]);
        ck::find::copyToArray(applied.typeCustomExtensions, result.values[15].c_str());
        applied.datePreset = static_cast<ck::find::GuidedDatePreset>(result.selected[16]);
        ck::find::copyToArray(applied.dateFrom, result.values[17].c_str());
        ck::find::copyToArray(applied.dateTo, result.values[18].c_str());
        applied.sizePreset = static_cast<ck::find::GuidedSizePreset>(result.selected[19]);
        ck::find::copyToArray(applied.sizePrimary, result.values[20].c_str());
        ck::find::copyToArray(applied.sizeSecondary, result.values[21].c_str());
        applied.sizeUseDecimalUnits = result.checked[22];
        applied.includePermissionAudit = result.checked[23];
        applied.includeTraversalFineTune = result.checked[24];
        applied.includeActionTweaks = result.checked[25];
        applied.listMatches = result.checked[26];
        applied.deleteMatches = result.checked[27];
        ck::find::applyGuidedStateToSpecification(applied, specification_);
        // The form can only write the two fixed native identifiers. Selecting
        // Disabled preserves an unknown legacy value for storage compatibility
        // but leaves it non-executable.
        if (result.selected[28] == 1 || result.selected[28] == 2)
        {
            specification_.enableActionOptions = true;
            specification_.actionOptions.execEnabled = true;
            specification_.actionOptions.execUsePlus = false;
            specification_.actionOptions.execVariant = ck::find::ActionOptions::ExecVariant::Exec;
            specification_.actionOptions.deleteMatches = false;
            ck::find::copyToArray(specification_.actionOptions.execCommand,
                                  result.selected[28] == 1 ? "ckvision.file-info" : "ckvision.sha256");
        }
        else if (existing_custom_command == "ckvision.file-info" || existing_custom_command == "ckvision.sha256")
        {
            specification_.actionOptions.execEnabled = false;
        }
        show_preview();
    });
}

void FindApp::show_save_dialog()
{
    search_dialog_.reset();
    const std::string current_name = ck::find::normaliseSpecificationName(ck::find::bufferToString(specification_.specName));
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Save search";
    dialog.fields.push_back({"&Name:", current_name.empty() ? "Unnamed" : current_name,
                             [](const std::string &value) {
                                 return !ck::find::normaliseSpecificationName(value).empty();
                             }});
    dialog.buttons.push_back({"&Save", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    search_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    search_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.size() != 1)
            return;
        const std::string name = ck::find::normaliseSpecificationName(result.values.front());
        if (name.empty())
            return;

        ck::find::SearchSpecification saved = specification_;
        ck::find::copyToArray(saved.specName, name.c_str());
        if (!specification_store_.save(saved, name))
        {
            present_text_window("Save search", "The saved-search store could not save '" + name + "'.");
            return;
        }
        specification_ = std::move(saved);
        present_text_window("Save search", "Saved search specification '" + name + "'.");
    });
}

void FindApp::show_load_dialog()
{
    const auto saved = specification_store_.list();
    if (saved.empty())
    {
        present_text_window("Load saved search", "No saved search specifications are available.");
        return;
    }

    std::ostringstream choices;
    choices << "Available saved searches:\n";
    for (const auto &item : saved)
        choices << "  " << item.name << '\n';
    present_text_window("Saved searches", choices.str());

    search_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Load saved search";
    dialog.fields.push_back({"&Name:", saved.front().name,
                             [](const std::string &value) {
                                 return !ck::find::normaliseSpecificationName(value).empty();
                             }});
    dialog.buttons.push_back({"&Load", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    search_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    search_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.size() != 1)
            return;
        const std::string name = ck::find::normaliseSpecificationName(result.values.front());
        if (const auto loaded = specification_store_.load(name))
        {
            specification_ = *loaded;
            show_preview();
            present_text_window("Load saved search", "Loaded search specification '" + name + "'.");
            return;
        }
        present_text_window("Load saved search", "No saved search specification named '" + name + "' was found.");
    });
}

std::string FindApp::command_preview() const
{
    const bool custom_command = specification_.enableActionOptions && specification_.actionOptions.execEnabled;
    // The framework-neutral preview has no knowledge of the native sandbox
    // template. Omit its legacy exec fragment so the only displayed custom
    // action is the exact direct argv supplied by the executor below.
    const auto command = ck::find::buildFindCommand(specification_, !custom_command);
    std::ostringstream output;
    for (std::size_t index = 0; index < command.size(); ++index)
    {
        if (index != 0)
            output << ' ';
        output << command[index];
    }
    if (custom_command)
    {
        const FindCustomCommandCapability capability = execution_service_.custom_command_capability(specification_);
        output << "\n\nSandboxed custom-command argv:\n";
        if (!capability.available)
        {
            output << "Unavailable: " << capability.reason;
        }
        else
        {
            for (const std::string &argument : capability.argv_preview)
                output << '[' << argument << "] ";
        }
    }
    return output.str();
}

void FindApp::set_specification(ck::find::SearchSpecification specification)
{
    specification_ = std::move(specification);
}

void FindApp::show_preview()
{
    present_text_window("Find command preview", command_preview());
}

void FindApp::request_execution()
{
    if (execution_service_.running())
    {
        present_text_window("Find execution", "A search is already running. Use Cancel search before starting another.");
        return;
    }

    if (specification_.enableActionOptions && specification_.actionOptions.execEnabled)
    {
        if (specification_.actionOptions.deleteMatches)
        {
            present_text_window("Find execution",
                                "Custom commands cannot be combined with deletion. Run one explicitly confirmed action at a time.");
            return;
        }
        const FindCustomCommandCapability capability = execution_service_.custom_command_capability(specification_);
        if (!capability.available)
        {
            present_text_window("Find execution", "Custom command unavailable: " + capability.reason);
            return;
        }

        std::ostringstream preview;
        for (const std::string &argument : capability.argv_preview)
            preview << '[' << argument << "] ";
        custom_command_confirmation_.reset();
        custom_command_confirmation_.emplace(ckv::widgets::present_message_box(
            application_, shell_->desktop(), shell_->roles(),
            {ckv::widgets::MessageBoxKind::Warning,
             "Run sandboxed custom command",
             "Run the fixed, non-interactive command for matching paths? It has a five-second per-path limit, a 16 KiB output limit, and stops after 64 paths.\n\n"
                 "Direct argv:\n" + preview.str(),
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        custom_command_confirmation_->set_completion_handler([this](ckv::widgets::MessageBoxResult result) {
            if (result == ckv::widgets::MessageBoxResult::Yes)
                start_execution(false);
        });
        return;
    }

    if (specification_.enableActionOptions && specification_.actionOptions.deleteMatches)
    {
        destructive_confirmation_.reset();
        destructive_confirmation_.emplace(ckv::widgets::present_message_box(
            application_, shell_->desktop(), shell_->roles(),
            {ckv::widgets::MessageBoxKind::Warning,
             "Delete matched files",
             "Delete matching regular files and symbolic links under '" +
                 ck::find::bufferToString(specification_.startLocation) +
                 "'? Directories and custom commands will not be executed. This cannot be undone.",
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        destructive_confirmation_->set_completion_handler([this](ckv::widgets::MessageBoxResult result) {
            if (result == ckv::widgets::MessageBoxResult::Yes)
                start_execution(true);
        });
        return;
    }

    start_execution(false);
}

void FindApp::start_execution(bool delete_matched_files)
{

    last_execution_result_.reset();
    const std::weak_ptr<void> lifetime = lifetime_;
    auto *const application = &application_;
    auto *const self = this;
    execution_service_.start(specification_, delete_matched_files,
                             [application, lifetime, self](ck::find::SearchExecutionResult result) mutable {
        if (lifetime.expired())
            return;
        application->post([self, lifetime, result = std::move(result)]() mutable {
            if (lifetime.expired())
                return;
            self->complete_execution(std::move(result));
        });
    });
    const bool custom_command = specification_.enableActionOptions && specification_.actionOptions.execEnabled;
    present_text_window("Find execution", delete_matched_files
                                             ? "Confirmed deletion is running. Only matching regular files and symbolic links can be removed."
                                             : custom_command ? "Sandboxed custom command execution is running for matching paths."
                                                              : "Search is running without actions.");
}

void FindApp::cancel_execution()
{
    if (!execution_service_.running())
    {
        present_text_window("Find execution", "There is no running search to cancel.");
        return;
    }
    execution_service_.cancel();
    present_text_window("Find execution", "Cancellation requested. The search will stop at its next traversal boundary.");
}

void FindApp::complete_execution(ck::find::SearchExecutionResult result)
{
    std::ostringstream output;
    if (result.cancelled)
        output << "Search cancelled after " << result.matchCount << " match(es).\n";
    else if (result.exitCode == 0)
        output << "Search completed with " << result.matchCount << " match(es).\n";
    else
        output << "Search completed with exit code " << result.exitCode << " after " << result.matchCount << " match(es).\n";

    if (result.deletedCount != 0 || result.failedDeletionCount != 0)
    {
        output << "Deleted " << result.deletedCount << " matching file(s) or symbolic link(s).\n";
        if (result.failedDeletionCount != 0)
            output << result.failedDeletionCount << " matching file(s) could not be deleted.\n";
    }

    if (result.customCommandInvocationCount != 0 || !result.customCommandAudit.empty())
    {
        output << result.customCommandInvocationCount << " sandboxed custom command invocation(s).\n";
        if (result.failedCustomCommandInvocationCount != 0)
            output << result.failedCustomCommandInvocationCount << " custom command invocation(s) did not complete successfully.\n";
        if (result.customCommandCancelled)
            output << "Custom command cancellation was requested.\n";
        if (result.customCommandTimedOut)
            output << "At least one custom command exceeded its five-second limit.\n";
        if (result.customCommandOutputTruncated)
            output << "At least one custom command exceeded its 16 KiB output limit.\n";
        if (!result.customCommandAudit.empty())
            output << "Custom command audit: " << result.customCommandAudit << '\n';
    }

    for (const auto &match : result.matches)
        output << match.string() << '\n';
    if (result.matchCount > result.matches.size())
        output << "Only the first " << result.matches.size() << " match(es) are displayed.\n";

    last_execution_result_ = result;
    present_text_window("Find results", output.str());
}

bool FindApp::execution_running() const noexcept
{
    return execution_service_.running();
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
