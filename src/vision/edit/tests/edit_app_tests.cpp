#include "edit_app.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

#include "markdown_normalization.hpp"

namespace
{
void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
}

int main()
{
    ckv::widgets::SyntaxProfileRegistry profiles;
    require(ck::vision::register_markdown_syntax_profile(profiles),
            "The suite Markdown profile must register in an application-owned registry.");
    const auto *markdown = profiles.find("markdown");
    require(markdown != nullptr && markdown->detect({std::nullopt, "notes.md", {}, {}}).score > 0,
            "The Markdown profile must detect Markdown filenames without global registration.");
    const auto heading = markdown->highlight_line("# Heading", "");
    require(!heading.spans.empty() && heading.spans.front().kind == ckv::widgets::SyntaxTokenKind::Keyword,
            "The Markdown profile must style headings through syntax tokens.");
    const auto fence = markdown->highlight_line("```cpp", "");
    const auto code = markdown->highlight_line("int value = 1;", fence.next_state);
    require(!fence.next_state.empty() && !code.spans.empty() &&
                code.spans.front().kind == ckv::widgets::SyntaxTokenKind::String,
            "The Markdown profile must carry fenced-code state between lines.");

    const std::string normalized_markdown = ck::vision::normalise_markdown_whitespace(
        "Intro   \n\n```cpp\nint value = 1;   \n\n```\n\n    keep trailing   \n\nAfter\t \n\n\n");
    require(normalized_markdown == "Intro  \n\n```cpp\nint value = 1;   \n\n```\n\n    keep trailing   \n\nAfter\n",
            "Markdown normalization must preserve fenced and indented code verbatim while normalizing ordinary prose.");

    ckv::MemoryFileSystem files;
    files.add_file("/notes.md", "# Notes\n\nNative editor test.\n");
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ck::vision::EditApp editor(application, files);
    require(editor.open_file("/notes.md"), "The native editor must open through its injected file service.");
    require(editor.path() == "/notes.md" && editor.document().text().find("Native editor") != std::string::npos,
            "The native editor must retain the loaded document and its path.");
    require(editor.syntax_profile() == "markdown",
            "Markdown documents must select the suite-owned syntax profile through ckVision's registry.");
    editor.document().set_text("# Heading  \n\nBody \n\n\n");
    editor.document().mark_clean();
    require(application.execute_command(editor.normalise_markdown_command()),
            "Markdown normalization must dispatch through the command registry.");
    require(editor.document().text() == "# Heading  \n\nBody\n",
            "Markdown normalization must preserve hard breaks while removing accidental whitespace in one transaction.");
    require(editor.document().modified(), "Markdown normalization must remain an undoable document edit.");

    editor.document().set_text("caf\xC3\xA9\nHeading\n```cpp\n# code\n```\n");
    const auto inline_begin = editor.document().position_at_byte(0);
    const auto inline_end = editor.document().position_at_byte(5);
    require(inline_begin.has_value() && inline_end.has_value() &&
                editor.editor_view()->set_selection({*inline_begin, *inline_end}),
            "The native editor must accept a current selection for a controller-owned Markdown transform.");
    require(application.execute_command(editor.bold_command()),
            "Bold Markdown must dispatch through the command registry.");
    require(editor.document().text().starts_with("**caf\xC3\xA9**\n") &&
                editor.editor_view()->selection().has_value() &&
                editor.document().text(*editor.editor_view()->selection()) == "caf\xC3\xA9",
            "Bold Markdown must preserve the selected Unicode content after one document transaction.");
    require(application.execute_command(editor.bold_command()) && editor.document().text().starts_with("caf\xC3\xA9\n"),
            "Applying the same inline Markdown command again must remove its exact delimiter.");

    const auto heading_position = editor.document().position_at_byte(6);
    require(heading_position.has_value() && editor.editor_view()->set_selection({*heading_position, *heading_position}),
            "The native editor must position a zero-width heading transform at the current line.");
    require(application.execute_command(editor.heading_command(2)),
            "Heading Markdown must dispatch through the command registry.");
    require(editor.document().text() == "caf\xC3\xA9\n## Heading\n```cpp\n# code\n```\n",
            "Heading transformation must leave fenced code unchanged.");
    require(application.execute_command(editor.heading_command(2)) &&
                editor.document().text() == "caf\xC3\xA9\nHeading\n```cpp\n# code\n```\n",
            "Applying the current heading level again must remove its ATX marker.");

    editor.document().set_text("Plan\n```cpp\n- [ ] code\n```\n");
    const auto task_position = editor.document().position_at_byte(0);
    require(task_position.has_value() && editor.editor_view()->set_selection({*task_position, *task_position}),
            "The native editor must position a zero-width task transform at the current line.");
    require(application.execute_command(editor.toggle_task_command()) &&
                editor.document().text() == "- [ ] Plan\n```cpp\n- [ ] code\n```\n",
            "Task transforms must promote ordinary text while leaving fenced code unchanged.");
    require(application.execute_command(editor.toggle_task_command()) &&
                editor.document().text() == "- [x] Plan\n```cpp\n- [ ] code\n```\n",
            "Repeating a task command must toggle its checked state through the command registry.");

    editor.document().set_text("Note\n```cpp\ncode\n```\n");
    const auto quote_position = editor.document().position_at_byte(0);
    require(quote_position.has_value() && editor.editor_view()->set_selection({*quote_position, *quote_position}),
            "The native editor must position a zero-width quote transform at the current line.");
    require(application.execute_command(editor.toggle_quote_command()) &&
                editor.document().text() == "> Note\n```cpp\ncode\n```\n",
            "Quote transforms must add a quote level without rewriting fenced code.");
    require(application.execute_command(editor.toggle_quote_command()) &&
                editor.document().text() == "Note\n```cpp\ncode\n```\n",
            "Repeating a quote command must remove its added quote level through the command registry.");

    editor.document().set_text("Note\n```cpp\ncode\n```\n");
    const auto bullet_position = editor.document().position_at_byte(0);
    require(bullet_position.has_value() && editor.editor_view()->set_selection({*bullet_position, *bullet_position}),
            "The native editor must position a zero-width list transform at the current line.");
    require(application.execute_command(editor.toggle_bullet_list_command()) &&
                editor.document().text() == "- Note\n```cpp\ncode\n```\n",
            "Bullet-list transforms must add a list marker without rewriting fenced code.");
    require(application.execute_command(editor.toggle_bullet_list_command()) &&
                editor.document().text() == "Note\n```cpp\ncode\n```\n",
            "Repeating a bullet-list command must remove its marker through the command registry.");

    editor.document().set_text("First\nSecond\n");
    const auto list_begin = editor.document().position_at_byte(0);
    const auto list_end = editor.document().position_at_byte(editor.document().text().size());
    require(list_begin.has_value() && list_end.has_value() && editor.editor_view()->set_selection({*list_begin, *list_end}),
            "The native editor must select multiple lines for an ordered-list transform.");
    require(application.execute_command(editor.toggle_ordered_list_command()) &&
                editor.document().text() == "1. First\n2. Second\n",
            "Ordered-list transforms must number selected ordinary lines sequentially.");
    require(application.execute_command(editor.toggle_ordered_list_command()) && editor.document().text() == "First\nSecond\n",
            "Repeating an ordered-list command must restore plain selected lines through the command registry.");

    require(application.execute_command(editor.save_command()), "Save must dispatch through the command registry.");
    require(!editor.document().modified(), "A successful save must establish a clean editor revision.");
    require(application.execute_command(editor.save_as_command()), "Save As must dispatch through the command registry.");
    application.step(0);
    require(application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Escape, ckv::Modifier::None, ""}}),
            "The test Save As presentation must remain dismissible without changing the current document.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native editor must render headlessly.");

    editor.document().set_text("Local edits that must not overwrite the external version.\n");
    files.add_file("/notes.md", "# External version\n");
    require(application.execute_command(editor.save_command()), "A conflicting save must dispatch through the command registry.");
    application.step(0);
    require(application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Tab, ckv::Modifier::None, ""}}) &&
                application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}}),
            "The external-change resolution must require an explicit choice before discarding in-memory edits.");
    application.step(0);
    require(editor.document().text() == "# External version\n" && !editor.document().modified(),
            "Choosing Reload after an external save conflict must preserve the external file and replace local edits only by explicit choice.");

    editor.document().set_text("Unsaved replacement");
    require(!editor.request_close(ckv::widgets::EditorCloseChoice::Cancel),
            "Cancelling a close must retain an unsaved document.");
    require(editor.document().modified(), "Cancelling a close must preserve the document's modified state.");
    require(editor.request_close(ckv::widgets::EditorCloseChoice::Discard),
            "Discarding a close must use the editor controller's explicit close decision.");
}
