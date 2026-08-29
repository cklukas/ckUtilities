#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "find_app.hpp"

int main()
{
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::CoreFindSpecificationStore specifications;
    ck::vision::ThreadedFindExecutionService execution;
    ck::vision::FindApp find(application, specifications, execution);
    application.run();
    return 0;
}
