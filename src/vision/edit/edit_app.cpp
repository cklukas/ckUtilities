#include "edit_app.hpp"

#include "ck/vision/keymap.hpp"
#include "markdown_normalization.hpp"

#include "ck/edit/markdown_transformations.hpp"

#include <utility>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>

namespace ck::vision
{
namespace
{
using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;
}

EditApp::EditApp(ckv::ui::Application &application, ckv::FileSystem &files)
    : application_(application), files_(files), document_(std::make_shared<ckv::widgets::EditorDocument>()),
      command_scope_(application.commands())
{
    ckv::widgets::register_standard_syntax_profiles(profiles_);
    register_markdown_syntax_profile(profiles_);
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_editor_window();
}

void EditApp::declare_commands()
{
    open_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.open", [this] { open_file_dialog(); }));
    save_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.save", [this] { save(); }));
    save_as_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-edit", "ck.edit.save_as", [this] { show_save_as_dialog(); }));
    normalise_markdown_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-edit", "ck.edit.normalise_markdown", [this] { normalise_markdown(); }));
    reflow_markdown_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-edit", "ck.edit.reflow_markdown", [this] { reflow_markdown(); }));
    bold_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.bold", [this] {
            toggle_inline_markdown(ck::edit::MarkdownInlineStyle::Bold, "bold");
        }));
    italic_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.italic", [this] {
            toggle_inline_markdown(ck::edit::MarkdownInlineStyle::Italic, "italic");
        }));
    strikethrough_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-edit", "ck.edit.strikethrough", [this] {
            toggle_inline_markdown(ck::edit::MarkdownInlineStyle::Strikethrough, "strikethrough");
        }));
    inline_code_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.inline_code", [this] {
            toggle_inline_markdown(ck::edit::MarkdownInlineStyle::Code, "inline code");
        }));
    toggle_link_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.toggle_link", [this] { toggle_link_markdown(); }));
    toggle_task_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.toggle_task", [this] { toggle_task_markdown(); }));
    toggle_quote_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-edit", "ck.edit.toggle_quote", [this] { toggle_quote_markdown(); }));
    toggle_bullet_list_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-edit", "ck.edit.toggle_bullet_list", [this] {
            toggle_list_markdown(ck::edit::MarkdownListStyle::Bullet, "bullet list");
        }));
    toggle_ordered_list_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-edit", "ck.edit.toggle_ordered_list", [this] {
            toggle_list_markdown(ck::edit::MarkdownListStyle::Ordered, "ordered list");
        }));
    for (int level = 1; level <= 6; ++level)
    {
        heading_commands_[static_cast<std::size_t>(level - 1)] = command_scope_.own(declare_suite_command(
            application_.commands(), "ck-edit", "ck.edit.heading." + std::to_string(level),
            [this, level] { toggle_heading_markdown(level); }));
    }
}

SuiteShellOptions EditApp::make_shell_options() const
{
    return {.application_name = "ck Edit",
            .about_text = "A native ckVision document editor with explicit injected file operations.",
            .application_menus = {MenuBarItem{"&File", {
                MenuItem::command(CommandPresentation{open_command_, "&Open document..."}),
                MenuItem::command(CommandPresentation{save_command_, "&Save"}),
                MenuItem::command(CommandPresentation{save_as_command_, "Save &As..."}),
                MenuItem::command(CommandPresentation{normalise_markdown_command_, "&Normalize Markdown whitespace"}),
            }}, MenuBarItem{"&Format", {
                MenuItem::command(CommandPresentation{reflow_markdown_command_, "&Reflow Markdown paragraph"}),
                MenuItem::command(CommandPresentation{bold_command_, "&Bold selection"}),
                MenuItem::command(CommandPresentation{italic_command_, "&Italic selection"}),
                MenuItem::command(CommandPresentation{strikethrough_command_, "&Strikethrough selection"}),
                MenuItem::command(CommandPresentation{inline_code_command_, "Inline &code selection"}),
                MenuItem::command(CommandPresentation{toggle_link_command_, "Insert or remove &link..."}),
                MenuItem::command(CommandPresentation{toggle_task_command_, "Toggle &task"}),
                MenuItem::command(CommandPresentation{toggle_quote_command_, "Toggle &quote"}),
                MenuItem::command(CommandPresentation{toggle_bullet_list_command_, "Toggle &bullet list"}),
                MenuItem::command(CommandPresentation{toggle_ordered_list_command_, "Toggle &ordered list"}),
                MenuItem::separator(),
                MenuItem::command(CommandPresentation{heading_commands_[0], "Heading &1"}),
                MenuItem::command(CommandPresentation{heading_commands_[1], "Heading &2"}),
                MenuItem::command(CommandPresentation{heading_commands_[2], "Heading &3"}),
                MenuItem::command(CommandPresentation{heading_commands_[3], "Heading &4"}),
                MenuItem::command(CommandPresentation{heading_commands_[4], "Heading &5"}),
                MenuItem::command(CommandPresentation{heading_commands_[5], "Heading &6"}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{open_command_, "&Open"}, 20},
                StatusLineItem{CommandPresentation{save_command_, "&Save"}, 20},
                StatusLineItem{CommandPresentation{normalise_markdown_command_, "&Normalize"}, 25},
                StatusLineItem{CommandPresentation{bold_command_, "&Bold"}, 20},
            }};
}

void EditApp::create_editor_window()
{
    auto window = std::make_unique<ckv::widgets::EditorWindow>("ck Edit", document_, files_, &profiles_);
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{60, 16});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->editor().set_show_line_numbers(true);
    window->close_request = [this] {
        if (closing_after_explicit_choice_)
            return true;
        if (window_ == nullptr || !window_->controller().modified())
            return true;
        show_close_confirmation();
        return false;
    };
    window->on_closed = [this] { close_editor_window(); };
    window_ = static_cast<ckv::widgets::EditorWindow *>(shell_->desktop().add_window(std::move(window)));
    application_.set_focus(&window_->editor());
}

bool EditApp::request_close(ckv::widgets::EditorCloseChoice choice)
{
    if (window_ == nullptr)
        return true;
    const auto result = window_->request_close(choice);
    if (result != ckv::widgets::EditorFileStatus::Ok)
    {
        if (choice != ckv::widgets::EditorCloseChoice::Cancel)
            show_message(ckv::widgets::MessageBoxKind::Error, "Close document", status_message(result));
        return false;
    }
    closing_after_explicit_choice_ = true;
    const bool closed = window_->close();
    closing_after_explicit_choice_ = false;
    return closed;
}

void EditApp::close_editor_window()
{
    if (window_ == nullptr)
        return;
    ckv::widgets::EditorWindow *const closing = window_;
    window_ = nullptr;
    shell_->desktop().remove_window(closing);
    application_.request_quit();
}

void EditApp::show_close_confirmation()
{
    if (window_ == nullptr || !window_->controller().modified())
        return;
    close_confirmation_.reset();
    close_confirmation_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(),
        {ckv::widgets::MessageBoxKind::Warning,
         "Unsaved document",
         "Save changes before closing? Choose No to discard the unsaved changes.",
         ckv::widgets::MessageBoxButtons::YesNoCancel}));
    close_confirmation_->set_completion_handler([this](ckv::widgets::MessageBoxResult result) {
        switch (result)
        {
        case ckv::widgets::MessageBoxResult::Yes:
            request_close(ckv::widgets::EditorCloseChoice::Save);
            break;
        case ckv::widgets::MessageBoxResult::No:
            request_close(ckv::widgets::EditorCloseChoice::Discard);
            break;
        case ckv::widgets::MessageBoxResult::Ok:
        case ckv::widgets::MessageBoxResult::Cancel:
            break;
        }
    });
}

bool EditApp::open_file(const std::string &path)
{
    if (window_ == nullptr)
        return false;
    const auto result = window_->open(path);
    if (result == ckv::widgets::EditorFileStatus::Ok)
        return true;
    show_message(ckv::widgets::MessageBoxKind::Error, "Open document", status_message(result));
    return false;
}

void EditApp::open_file_dialog()
{
    open_dialog_.reset();
    open_dialog_.emplace(ckv::widgets::present_file_dialog(
        ckv::widgets::FileDialogMode::Open, ".", files_, {}, application_, shell_->desktop(), shell_->roles()));
    open_dialog_->set_completion_handler([this](ckv::widgets::FileDialogResult result) {
        if (result.accepted)
            open_file(result.path);
    });
}

void EditApp::save()
{
    if (window_ == nullptr)
        return;
    const auto result = window_->save();
    if (result == ckv::widgets::EditorFileStatus::Conflict)
    {
        show_save_conflict_resolution();
        return;
    }
    if (result != ckv::widgets::EditorFileStatus::Ok)
        show_message(ckv::widgets::MessageBoxKind::Error, "Save document", status_message(result));
}

void EditApp::show_save_as_dialog()
{
    if (window_ == nullptr)
        return;
    save_as_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Save document as";
    dialog.fields.push_back({"&Path:", window_->controller().path(), [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Save", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    save_as_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    save_as_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.empty() || window_ == nullptr)
            return;
        const auto status = window_->save_as(result.values[0], ckv::widgets::EditorSaveAsPolicy::FailIfExists);
        if (status != ckv::widgets::EditorFileStatus::Ok)
            show_message(ckv::widgets::MessageBoxKind::Error, "Save document", status_message(status));
    });
}

void EditApp::show_save_conflict_resolution()
{
    if (window_ == nullptr)
        return;
    save_conflict_confirmation_.reset();
    save_conflict_confirmation_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(),
        {ckv::widgets::MessageBoxKind::Warning,
         "Document changed on disk",
         "The document was changed outside ck Edit, so it was not overwritten. Choose Yes to save your edits to a separate path, "
         "No to reload the on-disk version and discard your in-memory edits, or Cancel to keep editing.",
         ckv::widgets::MessageBoxButtons::YesNoCancel}));
    save_conflict_confirmation_->set_completion_handler([this](ckv::widgets::MessageBoxResult result) {
        switch (result)
        {
        case ckv::widgets::MessageBoxResult::Yes:
            show_save_as_dialog();
            break;
        case ckv::widgets::MessageBoxResult::No:
            reload_after_save_conflict();
            break;
        case ckv::widgets::MessageBoxResult::Ok:
        case ckv::widgets::MessageBoxResult::Cancel:
            break;
        }
    });
}

void EditApp::reload_after_save_conflict()
{
    if (window_ == nullptr)
        return;
    const std::string current_path = window_->controller().path();
    const auto status = window_->open(current_path, {.modified_document = ckv::widgets::EditorOpenModifiedPolicy::Discard});
    if (status != ckv::widgets::EditorFileStatus::Ok)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Reload document", status_message(status));
        return;
    }
    window_->set_footer("Reloaded the externally changed document; in-memory edits were discarded by your choice.");
}

ckv::widgets::EditorDocument &EditApp::document() noexcept
{
    return *document_;
}

const ckv::widgets::EditorDocument &EditApp::document() const noexcept
{
    return *document_;
}

std::string EditApp::path() const
{
    return window_ == nullptr ? std::string{} : window_->controller().path();
}

std::string EditApp::syntax_profile() const
{
    return window_ == nullptr ? std::string{} : window_->editor().profile_id();
}

bool EditApp::normalise_markdown()
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown normalization is available for Markdown documents only.");
        return false;
    }

    const std::string original = document_->text();
    const std::string normalised = normalise_markdown_whitespace(original);
    if (normalised == original)
    {
        window_->set_footer("Markdown whitespace is already normalized.");
        return true;
    }

    auto transaction = document_->transaction();
    transaction.replace({document_->begin(), document_->end()}, normalised);
    if (!document_->commit(std::move(transaction)))
    {
        window_->set_footer("Could not normalize Markdown whitespace.");
        return false;
    }
    window_->set_footer("Normalized Markdown whitespace.");
    return true;
}

void EditApp::reflow_markdown()
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown reflow is available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    const ckv::widgets::DocumentPosition cursor = window_->editor().cursor();
    const ck::edit::MarkdownByteRange range = selected
                                                  ? ck::edit::MarkdownByteRange{selected->begin.byte, selected->end.byte}
                                                  : ck::edit::MarkdownByteRange{cursor.byte, cursor.byte};
    const auto transform = ck::edit::reflow_markdown_paragraphs(document_->text(), range);
    if (!transform || !commit_markdown_transform(*transform, "Reflowed Markdown paragraph to 80 columns."))
        window_->set_footer("Could not reflow a Markdown paragraph at this location.");
}

ckv::ui::CommandId EditApp::heading_command(int level) const noexcept
{
    if (level < 1 || level > static_cast<int>(heading_commands_.size()))
        return ckv::ui::kInvalidCommand;
    return heading_commands_[static_cast<std::size_t>(level - 1)];
}

bool EditApp::markdown_document() const noexcept
{
    return window_ != nullptr && window_->editor().profile_id() == "markdown";
}

void EditApp::toggle_inline_markdown(ck::edit::MarkdownInlineStyle style, std::string_view label)
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown formatting is available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    if (!selected)
    {
        window_->set_footer("Select text before applying Markdown formatting.");
        return;
    }

    const auto transform = ck::edit::toggle_markdown_inline_style(
        document_->text(), {selected->begin.byte, selected->end.byte}, style);
    if (!transform || !commit_markdown_transform(*transform, "Updated " + std::string(label) + " Markdown."))
        window_->set_footer("Could not apply Markdown formatting to this selection.");
}

void EditApp::toggle_heading_markdown(int level)
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown headings are available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    const ckv::widgets::DocumentPosition cursor = window_->editor().cursor();
    const ck::edit::MarkdownByteRange range = selected
                                                  ? ck::edit::MarkdownByteRange{selected->begin.byte, selected->end.byte}
                                                  : ck::edit::MarkdownByteRange{cursor.byte, cursor.byte};
    const auto transform = ck::edit::toggle_markdown_heading(document_->text(), range, level);
    if (!transform || !commit_markdown_transform(*transform, "Toggled Markdown heading " + std::to_string(level) + "."))
        window_->set_footer("Could not toggle a Markdown heading at this location.");
}

void EditApp::toggle_task_markdown()
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown tasks are available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    const ckv::widgets::DocumentPosition cursor = window_->editor().cursor();
    const ck::edit::MarkdownByteRange range = selected
                                                  ? ck::edit::MarkdownByteRange{selected->begin.byte, selected->end.byte}
                                                  : ck::edit::MarkdownByteRange{cursor.byte, cursor.byte};
    const auto transform = ck::edit::toggle_markdown_task(document_->text(), range);
    if (!transform || !commit_markdown_transform(*transform, "Toggled Markdown task state."))
        window_->set_footer("Could not toggle a Markdown task at this location.");
}

void EditApp::toggle_quote_markdown()
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown quotes are available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    const ckv::widgets::DocumentPosition cursor = window_->editor().cursor();
    const ck::edit::MarkdownByteRange range = selected
                                                  ? ck::edit::MarkdownByteRange{selected->begin.byte, selected->end.byte}
                                                  : ck::edit::MarkdownByteRange{cursor.byte, cursor.byte};
    const auto transform = ck::edit::toggle_markdown_quote(document_->text(), range);
    if (!transform || !commit_markdown_transform(*transform, "Toggled Markdown quote level."))
        window_->set_footer("Could not toggle a Markdown quote at this location.");
}

void EditApp::toggle_list_markdown(ck::edit::MarkdownListStyle style, std::string_view label)
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown lists are available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    const ckv::widgets::DocumentPosition cursor = window_->editor().cursor();
    const ck::edit::MarkdownByteRange range = selected
                                                  ? ck::edit::MarkdownByteRange{selected->begin.byte, selected->end.byte}
                                                  : ck::edit::MarkdownByteRange{cursor.byte, cursor.byte};
    const auto transform = ck::edit::toggle_markdown_list(document_->text(), range, style);
    if (!transform || !commit_markdown_transform(*transform, "Toggled Markdown " + std::string(label) + "."))
        window_->set_footer("Could not toggle a Markdown list at this location.");
}

void EditApp::toggle_link_markdown()
{
    if (!markdown_document())
    {
        if (window_ != nullptr)
            window_->set_footer("Markdown links are available for Markdown documents only.");
        return;
    }

    const auto selected = window_->editor().selection();
    if (!selected)
    {
        window_->set_footer("Select link text before inserting or removing a Markdown link.");
        return;
    }

    const ck::edit::MarkdownByteRange range{selected->begin.byte, selected->end.byte};
    if (const auto transform = ck::edit::toggle_markdown_link(document_->text(), range, ""))
    {
        if (!commit_markdown_transform(*transform, "Removed Markdown link."))
            window_->set_footer("Could not remove the Markdown link at this selection.");
        return;
    }
    show_link_destination_dialog();
}

void EditApp::show_link_destination_dialog()
{
    if (!markdown_document())
        return;

    link_destination_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Insert Markdown link";
    dialog.fields.push_back({"&Destination:", "", [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Insert", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    link_destination_dialog_.emplace(
        ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    link_destination_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.size() != 1U || !markdown_document())
            return;

        const auto selected = window_->editor().selection();
        if (!selected)
            return;
        const auto transform = ck::edit::toggle_markdown_link(
            document_->text(), {selected->begin.byte, selected->end.byte}, result.values.front());
        if (!transform || !commit_markdown_transform(*transform, "Inserted Markdown link."))
            window_->set_footer("Could not insert a Markdown link with that destination.");
    });
}

bool EditApp::commit_markdown_transform(const ck::edit::MarkdownTransformEdit &transform,
                                        std::string_view success_message)
{
    const auto begin = document_->position_at_byte(transform.replaced.begin);
    const auto end = document_->position_at_byte(transform.replaced.end);
    if (!begin || !end)
        return false;

    auto transaction = document_->transaction();
    transaction.replace({*begin, *end}, transform.replacement);
    if (!document_->commit(std::move(transaction)))
        return false;

    const auto selected_begin = document_->position_at_byte(transform.selection.begin);
    const auto selected_end = document_->position_at_byte(transform.selection.end);
    if (!selected_begin || !selected_end || !window_->editor().set_selection({*selected_begin, *selected_end}))
        return false;

    window_->set_footer(std::string(success_message));
    return true;
}

void EditApp::show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message)
{
    message_box_.reset();
    message_box_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(), {kind, std::move(title), std::move(message), ckv::widgets::MessageBoxButtons::Ok}));
    message_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

std::string EditApp::status_message(ckv::widgets::EditorFileStatus status)
{
    switch (status)
    {
    case ckv::widgets::EditorFileStatus::NotFound: return "The document could not be found.";
    case ckv::widgets::EditorFileStatus::InvalidText: return "The document is not valid UTF-8.";
    case ckv::widgets::EditorFileStatus::Conflict: return "The document changed on disk or has unsaved changes.";
    case ckv::widgets::EditorFileStatus::NoPath: return "Choose a path with Save As before saving.";
    case ckv::widgets::EditorFileStatus::Error: return "The file operation failed.";
    case ckv::widgets::EditorFileStatus::Ok: break;
    }
    return {};
}

} // namespace ck::vision
