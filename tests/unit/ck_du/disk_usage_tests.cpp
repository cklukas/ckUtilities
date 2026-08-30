#include <gtest/gtest.h>

#include "disk_usage_core.hpp"
#include "disk_usage_options.hpp"

#include "ck/options.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

struct UnitGuard
{
    ck::du::SizeUnit previous = ck::du::getCurrentUnit();
    ~UnitGuard() { ck::du::setCurrentUnit(previous); }
};

struct SortGuard
{
    ck::du::SortKey previous = ck::du::getCurrentSortKey();
    ~SortGuard() { ck::du::setCurrentSortKey(previous); }
};

} // namespace

TEST(DiskUsageCore, FormatsSizesAcrossUnits)
{
    UnitGuard guard;

    EXPECT_EQ(ck::du::formatSize(512, ck::du::SizeUnit::Bytes), "512 B");
    EXPECT_EQ(ck::du::formatSize(1024, ck::du::SizeUnit::Kilobytes), "1.00 KB");
    EXPECT_EQ(ck::du::formatSize(1536, ck::du::SizeUnit::Kilobytes), "1.50 KB");
    EXPECT_EQ(ck::du::formatSize(1048576, ck::du::SizeUnit::Megabytes), "1.00 MB");

    ck::du::setCurrentUnit(ck::du::SizeUnit::Gigabytes);
    EXPECT_EQ(ck::du::getCurrentUnit(), ck::du::SizeUnit::Gigabytes);
    EXPECT_EQ(ck::du::formatSize(1073741824), "1.00 GB");
}

TEST(DiskUsageCore, ReportsSortKeys)
{
    SortGuard guard;
    EXPECT_STREQ(ck::du::sortKeyName(ck::du::SortKey::NameAscending), "Name (A→Z)");
    ck::du::setCurrentSortKey(ck::du::SortKey::SizeDescending);
    EXPECT_EQ(ck::du::getCurrentSortKey(), ck::du::SortKey::SizeDescending);
}

TEST(DiskUsageCore, ProvidesUnitLabels)
{
    EXPECT_STREQ(ck::du::unitName(ck::du::SizeUnit::Auto), "Auto");
    EXPECT_STREQ(ck::du::unitName(ck::du::SizeUnit::Terabytes), "Terabytes");
}

TEST(DiskUsageCore, ReportsCancelledDirectoryScans)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() /
                               ("ck-du-cancel-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    {
        std::ofstream file(directory / "item.txt");
        file << "data";
    }

    ck::du::BuildDirectoryTreeOptions options;
    options.cancelRequested = [] { return true; };
    const auto result = ck::du::buildDirectoryTree(directory, options);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.root, nullptr);
    fs::remove_all(directory);
}

TEST(DiskUsageOptions, RegistersExpectedDefinitions)
{
    ck::config::OptionRegistry registry("ck-du");
    ck::du::registerDiskUsageOptions(registry);

    EXPECT_TRUE(registry.hasOption("symlinkPolicy"));
    EXPECT_TRUE(registry.hasOption("ignorePatterns"));

    auto options = registry.listRegisteredOptions();
    std::vector<std::string> keys;
    keys.reserve(options.size());
    for (const auto &definition : options)
        keys.push_back(definition.key);

    EXPECT_NE(std::find(keys.begin(), keys.end(), "threshold"), keys.end());
}

TEST(DiskUsageOptions, BuildsScanOptionsFromProfile)
{
    ck::config::OptionRegistry registry("ck-du");
    ck::du::registerDiskUsageOptions(registry);
    registry.set("symlinkPolicy", ck::config::OptionValue(std::string("command-line")));
    registry.set("countHardLinksMultiple", ck::config::OptionValue(true));
    registry.set("ignoreNodump", ck::config::OptionValue(true));
    registry.set("reportErrors", ck::config::OptionValue(false));
    registry.set("threshold", ck::config::OptionValue(std::int64_t{4096}));
    registry.set("stayOnFilesystem", ck::config::OptionValue(true));
    registry.set("ignorePatterns", ck::config::OptionValue(std::vector<std::string>{"*.tmp", ".cache"}));

    const ck::du::BuildDirectoryTreeOptions options = ck::du::buildDirectoryTreeOptionsFromRegistry(registry);

    EXPECT_EQ(options.symlinkPolicy, ck::du::BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly);
    EXPECT_TRUE(options.followCommandLineSymlinks);
    EXPECT_TRUE(options.countHardLinksMultipleTimes);
    EXPECT_TRUE(options.ignoreNodumpFlag);
    EXPECT_FALSE(options.reportErrors);
    EXPECT_EQ(options.threshold, 4096);
    EXPECT_TRUE(options.stayOnFilesystem);
    EXPECT_EQ(options.ignoreMasks, (std::vector<std::string>{"*.tmp", ".cache"}));

    registry.set("symlinkPolicy", ck::config::OptionValue(std::string("always")));
    const ck::du::BuildDirectoryTreeOptions alwaysOptions = ck::du::buildDirectoryTreeOptionsFromRegistry(registry);
    EXPECT_EQ(alwaysOptions.symlinkPolicy, ck::du::BuildDirectoryTreeOptions::SymlinkPolicy::Always);
    EXPECT_FALSE(alwaysOptions.followCommandLineSymlinks);
}
