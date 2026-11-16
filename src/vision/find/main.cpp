// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/ui/application.hpp>

#include "ck/vision/keymap.hpp"
#include "find_app.hpp"
#include "host_about.hpp"
#include "native_host.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && args[1] == "--version")
    {
        std::printf("%s\n", ck::host::version_line("ck-find").c_str());
        return 0;
    }
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s [DIRECTORY] [PATTERN]\n", args.empty() ? "ck-find" : args[0].c_str());
        return 0;
    }

    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ck::host::NativeFileSystem files;

    std::string initial_directory = ".";
    std::string initial_pattern;
    if (args.size() > 1 && !args[1].empty())
    {
        if (files.is_directory(args[1]))
        {
            initial_directory = args[1];
            if (args.size() > 2)
                initial_pattern = args[2];
        }
        else
        {
            initial_pattern = args[1];
        }
    }

    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-find", application.commands(), keymap_persistence);
    ck::vision::CoreFindSpecificationStore specifications;
    ck::vision::ThreadedFindExecutionService execution;
    ck::vision::FindApp find(application, specifications, execution, files, initial_directory, initial_pattern);
    keymap.load();
    application.run();
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    return run(ck::host::native_arguments(argc, argv));
}
