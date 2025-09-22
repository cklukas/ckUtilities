#include <gtest/gtest.h>

#include "disk_usage_core.hpp"
#include "disk_usage_options.hpp"

#include "ck/options.hpp"

#include <algorithm>
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
