#include <string>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_filesystem.hpp>
#include <cvision/term/posix_terminal.hpp>
#include <cvision/term/terminal_clipboard.hpp>
#include <cvision/ui/application.hpp>

#include "ck/vision/keymap.hpp"
#include "json_view_app.hpp"

int main(int argc, char **argv)
{
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::term::TerminalClipboardWriter clipboard(terminal);
    ckv::ui::Application application(terminal, clock, clipboard);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-json-view", application.commands(), keymap_persistence);
    ckv::term::PosixFileSystem files;
    ck::vision::JsonViewApp json_view(application, files);
    keymap.load();

    for (int index = 1; index < argc; ++index)
        json_view.load_file(argv[index]);

    application.run();
    return 0;
}
