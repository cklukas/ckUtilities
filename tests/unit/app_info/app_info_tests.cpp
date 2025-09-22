#include <gtest/gtest.h>

#include "ck/app_info.hpp"

#include <algorithm>
#include <stdexcept>

namespace
{

bool containsToolId(std::span<const ck::appinfo::ToolInfo> tools, std::string_view id)
{
    return std::any_of(tools.begin(), tools.end(), [&](const ck::appinfo::ToolInfo &info) {
        return info.id == id;
    });
}

} // namespace

TEST(AppInfo, ListsAllKnownTools)
{
    auto tools = ck::appinfo::tools();
    ASSERT_GE(tools.size(), 5u);
    EXPECT_TRUE(containsToolId(tools, "ck-utilities"));
    EXPECT_TRUE(containsToolId(tools, "ck-edit"));
    EXPECT_TRUE(containsToolId(tools, "ck-du"));
    EXPECT_TRUE(containsToolId(tools, "ck-json-view"));
    EXPECT_TRUE(containsToolId(tools, "ck-config"));
}

TEST(AppInfo, RequireToolReturnsMatchingExecutable)
{
    const auto &info = ck::appinfo::requireTool("ck-du");
    EXPECT_EQ(info.id, "ck-du");
    EXPECT_EQ(info.executable, "ck-du");

    const auto &byExecutable = ck::appinfo::requireToolByExecutable("ck-json-view");
    EXPECT_EQ(byExecutable.id, "ck-json-view");
}

TEST(AppInfo, RequireToolThrowsForUnknownId)
{
    EXPECT_THROW(ck::appinfo::requireTool("does-not-exist"), std::runtime_error);
    EXPECT_THROW(ck::appinfo::requireToolByExecutable("missing"), std::runtime_error);
}
