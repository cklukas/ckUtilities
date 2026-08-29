#include <string_view>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_filesystem.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "edit_app.hpp"

int main(int argc, char **argv)
{
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::term::PosixFileSystem files;
    ckv::ui::Application application(terminal, clock);
    ck::vision::EditApp editor(application, files);
    if (argc > 1 && std::string_view(argv[1]) != "--help" && std::string_view(argv[1]) != "-h")
        editor.open_file(argv[1]);
    application.run();
    return 0;
}
