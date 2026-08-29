#include <cstdio>
#include <string_view>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_filesystem.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "ck/vision/keymap.hpp"
#include "edit_app.hpp"

int main(int argc, char **argv)
{
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::printf("Usage: %s [MARKDOWN_FILE]\n", argc > 0 ? argv[0] : "ck-edit-ckvision");
        return 0;
    }
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::term::PosixFileSystem files;
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-edit", application.commands(), keymap_persistence);
    ck::vision::EditApp editor(application, files);
    keymap.load();
    if (argc > 1)
        editor.open_file(argv[1]);
    application.run();
    return 0;
}
