#include "ck/find/search_backend.hpp"
#include "ck/find/search_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(SearchBackend, BuildsDefaultFindCommand)
{
    auto spec = ck::find::makeDefaultSpecification();
    auto command = ck::find::buildFindCommand(spec);
    std::vector<std::string> expected = {"find", ".", "-P", "!", "-name", ".*", "!", "-path", "*/.*", "-print"};
    EXPECT_EQ(command, expected);
}

TEST(SearchBackend, HiddenFilterRespectsConfiguration)
{
    auto spec = ck::find::makeDefaultSpecification();
    spec.includeHidden = true;
    auto command = ck::find::buildFindCommand(spec);
    std::vector<std::string> hidden = {"!", "-name", ".*"};
    EXPECT_EQ(std::search(command.begin(), command.end(), hidden.begin(), hidden.end()), command.end());

    spec.includeHidden = false;
    auto filtered = ck::find::buildFindCommand(spec);
    EXPECT_NE(std::search(filtered.begin(), filtered.end(), hidden.begin(), hidden.end()), filtered.end());
}

TEST(SearchBackend, NormalisesSpecificationNames)
{
    EXPECT_EQ(ck::find::normaliseSpecificationName("  Example Spec  "), "Example Spec");
    EXPECT_EQ(ck::find::normaliseSpecificationName("\t\n"), "");
}

TEST(SearchBackend, ExecutesSpecificationWithoutExternalFind)
{
    namespace fs = std::filesystem;
    fs::path tempDir = fs::temp_directory_path() /
                       fs::path("ck-find-backend-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir);

    fs::path textFile = tempDir / "example.txt";
    {
        std::ofstream stream(textFile);
        stream << "hello world" << std::endl;
    }

    fs::path hiddenFile = tempDir / ".ignored";
    {
        std::ofstream stream(hiddenFile);
        stream << "secret" << std::endl;
    }

    auto spec = ck::find::makeDefaultSpecification();
    std::snprintf(spec.startLocation.data(), spec.startLocation.size(), "%s", tempDir.c_str());
    std::snprintf(spec.searchText.data(), spec.searchText.size(), "%s", "hello");
    spec.textOptions.searchInContents = true;
    spec.textOptions.searchInFileNames = false;
    spec.includeHidden = false;

    ck::find::SearchExecutionOptions options;
    options.includeActions = false;
    options.captureMatches = true;
    options.filterContent = true;

    auto result = ck::find::executeSpecification(spec, options, nullptr, nullptr);
    EXPECT_EQ(result.exitCode, 0);
    ASSERT_EQ(result.matches.size(), 1u);
    EXPECT_EQ(result.matches.front(), textFile);

    fs::remove(hiddenFile);
    fs::remove(textFile);
    fs::remove_all(tempDir);
}
