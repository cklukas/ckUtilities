#include <cstdio>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "config_app.hpp"
#include "disk_usage_options.hpp"

int main()
{
    ck::config::OptionRegistry registry("ck-du");
    ck::du::registerDiskUsageOptions(registry);
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::ConfigApp config(application, registry);
    application.run();
    return 0;
}
