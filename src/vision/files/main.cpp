// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ck-files — the native host for the read-only two-pane Files application.
// Host-only concerns (arguments, terminal, user configuration and private
// cache roots) are resolved here; the controller and workers remain injected.
#include "files_app.hpp"

#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "operation_journal_file.hpp"

#include <cvision/ui/application.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using ck::files::NativeAbsolutePath;

std::optional<NativeAbsolutePath> absolute(const std::filesystem::path &path, const char *label) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value()) {
        std::fprintf(stderr, "ck-files: cannot resolve %s\n", label);
        return std::nullopt;
    }
    return std::move(value).take_value();
}

bool make_private_directory(const std::filesystem::path &path) {
    try {
        std::filesystem::create_directories(path);
#if !defined(_WIN32)
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                      std::filesystem::perm_options::replace);
#endif
        return std::filesystem::is_directory(path);
    } catch (const std::filesystem::filesystem_error &) {
        return false;
    }
}

int run(const std::vector<std::string> &args) {
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h")) {
        std::printf("Usage: %s [LEFT_DIRECTORY [RIGHT_DIRECTORY]]\n",
                    args.empty() ? "ck-files" : args[0].c_str());
        return 0;
    }
    if (args.size() > 3) {
        std::fprintf(stderr, "ck-files: expected at most two directory arguments\n");
        return 2;
    }

    const std::filesystem::path current = std::filesystem::current_path();
    const std::filesystem::path left_path = args.size() > 1 ? ck::host::utf8_path(args[1]) : current;
    const std::filesystem::path right_path = args.size() > 2 ? ck::host::utf8_path(args[2]) : left_path;
    const std::string config_root_text = ck::host::suite_config_root();
    if (config_root_text.empty()) {
        std::fprintf(stderr, "ck-files: cannot resolve a private configuration root\n");
        return 2;
    }
    const std::filesystem::path cache_root = ck::host::utf8_path(config_root_text) / "files";
    const std::filesystem::path left_index = cache_root / "index-left";
    const std::filesystem::path right_index = cache_root / "index-right";
    const std::filesystem::path left_completion = cache_root / "completion-left";
    const std::filesystem::path right_completion = cache_root / "completion-right";
    const std::filesystem::path operation_journals = cache_root / "operations";
    for (const auto &directory : {left_index, right_index, left_completion, right_completion, operation_journals}) {
        if (!make_private_directory(directory)) {
            std::fprintf(stderr, "ck-files: cannot create private cache directory\n");
            return 2;
        }
    }
    const auto operation_journal_root = absolute(operation_journals, "operation journal root");
    if (!operation_journal_root || !ck::files::prepare_native_operation_journal_root(*operation_journal_root).value()) {
        std::fprintf(stderr, "ck-files: cannot secure operation journal root\n");
        return 2;
    }

    const auto left = absolute(left_path, "left directory");
    const auto right = absolute(right_path, "right directory");
    const auto left_cache = absolute(left_index, "left cache");
    const auto right_cache = absolute(right_index, "right cache");
    const auto left_completion_cache = absolute(left_completion, "left completion cache");
    const auto right_completion_cache = absolute(right_completion, "right completion cache");
    if (!left || !right || !left_cache || !right_cache || !left_completion_cache || !right_completion_cache ||
        !operation_journal_root)
        return 2;

    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-files", application.commands(), keymap_persistence);
    ck::vision::FilesAppWake wake(application);
    std::array<std::unique_ptr<ck::vision::FilesPaneService>, 2> services;
    std::array<std::unique_ptr<ck::vision::FilesCompletionService>, 2> completion_services;
    auto planning_service = std::make_unique<ck::vision::FilesOperationPlanningService>(wake.callback());
    auto execution_service = std::make_unique<ck::vision::FilesOperationExecutionService>(wake.callback());
    services[0] = std::make_unique<ck::vision::FilesPaneService>(
        ck::vision::native_files_pane_backend(*left_cache), ck::vision::FilesPaneServiceLimits{}, wake.callback());
    services[1] = std::make_unique<ck::vision::FilesPaneService>(
        ck::vision::native_files_pane_backend(*right_cache), ck::vision::FilesPaneServiceLimits{}, wake.callback());
    completion_services[0] = std::make_unique<ck::vision::FilesCompletionService>(
        ck::vision::native_files_pane_backend(*left_completion_cache),
        ck::vision::FilesCompletionServiceLimits{}, wake.callback());
    completion_services[1] = std::make_unique<ck::vision::FilesCompletionService>(
        ck::vision::native_files_pane_backend(*right_completion_cache),
        ck::vision::FilesCompletionServiceLimits{}, wake.callback());
    ck::vision::FilesApp app(
        application,
        {services[0].get(), services[1].get()},
        wake,
        {*left, *right},
        [&application] { application.request_quit(); },
        ck::vision::ThemeScheme::Classic,
        {completion_services[0].get(), completion_services[1].get()}, planning_service.get(), execution_service.get(),
        std::make_shared<const NativeAbsolutePath>(*operation_journal_root));
    keymap.load();
    application.run();
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    return run(ck::host::native_arguments(argc, argv));
}
