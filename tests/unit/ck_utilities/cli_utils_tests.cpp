#include <gtest/gtest.h>

#include "ck/launcher/cli_utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
std::filesystem::path make_temp_executable()
{
    auto base = std::filesystem::temp_directory_path() /
                std::filesystem::path("ck_utilities_cli_utils_" +
                                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    auto exe = base / "ck-tool";
    std::ofstream file(exe);
    file << "#!/bin/sh\n";
    file.close();
    std::filesystem::permissions(exe, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    return exe;
}

void cleanup_temp(const std::filesystem::path &exe)
{
    std::error_code ec;
    std::filesystem::remove(exe, ec);
    std::filesystem::remove(exe.parent_path(), ec);
}
} // namespace

TEST(CkUtilitiesCliUtils, QuoteArgumentEscapesSingleQuotes)
{
    EXPECT_EQ(ck::launcher::quoteArgument("value"), "'value'");
    EXPECT_EQ(ck::launcher::quoteArgument("it's"), "'it'\\''s'");
}

TEST(CkUtilitiesCliUtils, WrapTextRespectsWidth)
{
    auto lines = ck::launcher::wrapText("alpha beta gamma delta", 10);
    ASSERT_GE(lines.size(), 2u);
    for (const auto &line : lines)
        EXPECT_LE(static_cast<int>(line.size()), 10);

    auto newlineSeparated = ck::launcher::wrapText("first\n\nsecond", 8);
    ASSERT_EQ(newlineSeparated.size(), 3u);
    EXPECT_TRUE(newlineSeparated[1].empty());
}

TEST(CkUtilitiesCliUtils, ResolveToolDirectoryReturnsParentPath)
{
    auto exe = make_temp_executable();
    auto parent = exe.parent_path();
    auto resolved = ck::launcher::resolveToolDirectory(exe.c_str());
    EXPECT_EQ(resolved, parent);
    cleanup_temp(exe);
}

TEST(CkUtilitiesCliUtils, LocateProgramPathFindsExecutable)
{
    auto exe = make_temp_executable();
    std::string executable = exe.filename().string();
    ck::appinfo::ToolInfo info{};
    info.id = "ck-util";
    info.executable = executable;
    info.displayName = "";
    info.shortDescription = "";
    info.aboutDescription = "";
    info.longDescription = "";

    auto located = ck::launcher::locateProgramPath(exe.parent_path(), info);
    ASSERT_TRUE(located.has_value());
    EXPECT_EQ(located->lexically_normal(), exe.lexically_normal());
    cleanup_temp(exe);
}
