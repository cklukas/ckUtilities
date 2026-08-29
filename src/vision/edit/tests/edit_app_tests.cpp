#include "edit_app.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

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
    require(application.execute_command(editor.save_command()), "Save must dispatch through the command registry.");
    require(application.execute_command(editor.save_as_command()), "Save As must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native editor must render headlessly.");

    editor.document().set_text("Unsaved replacement");
    require(!editor.request_close(ckv::widgets::EditorCloseChoice::Cancel),
            "Cancelling a close must retain an unsaved document.");
    require(editor.document().modified(), "Cancelling a close must preserve the document's modified state.");
    require(editor.request_close(ckv::widgets::EditorCloseChoice::Discard),
            "Discarding a close must use the editor controller's explicit close decision.");
}
