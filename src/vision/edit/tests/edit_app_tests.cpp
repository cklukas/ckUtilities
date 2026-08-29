#include "edit_app.hpp"

#include <cstdlib>
#include <iostream>

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
    ckv::MemoryFileSystem files;
    files.add_file("/notes.md", "# Notes\n\nNative editor test.\n");
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ck::vision::EditApp editor(application, files);
    require(editor.open_file("/notes.md"), "The native editor must open through its injected file service.");
    require(editor.path() == "/notes.md" && editor.document().text().find("Native editor") != std::string::npos,
            "The native editor must retain the loaded document and its path.");
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
