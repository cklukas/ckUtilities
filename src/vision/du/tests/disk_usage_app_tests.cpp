#include "disk_usage_app.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

namespace
{

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

ck::du::BuildDirectoryTreeResult make_snapshot()
{
    ck::du::BuildDirectoryTreeResult snapshot;
    snapshot.root = std::make_unique<ck::du::DirectoryNode>();
    snapshot.root->path = "/workspace";
    snapshot.root->stats = {.totalSize = 4096, .fileCount = 3, .directoryCount = 2};

    auto source = std::make_unique<ck::du::DirectoryNode>();
    source->path = "/workspace/src";
    source->parent = snapshot.root.get();
    source->stats = {.totalSize = 3072, .fileCount = 2, .directoryCount = 0};
    snapshot.root->children.push_back(std::move(source));

    auto docs = std::make_unique<ck::du::DirectoryNode>();
    docs->path = "/workspace/docs";
    docs->parent = snapshot.root.get();
    docs->stats = {.totalSize = 1024, .fileCount = 1, .directoryCount = 0};
    snapshot.root->children.push_back(std::move(docs));
    return snapshot;
}

} // namespace

int main()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ck::vision::DiskUsageApp disk_usage(application, make_snapshot());

    require(disk_usage.root_directory() != nullptr, "The native disk-usage app must retain the scan snapshot.");
    require(disk_usage.tree() != nullptr && disk_usage.table() != nullptr,
            "The native disk-usage app must compose a tree and selected-directory table.");
    require(disk_usage.table()->row_count() == 2,
            "The selected root directory must populate its child-directory table.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30},
            "The native disk-usage snapshot must render headlessly.");
}
