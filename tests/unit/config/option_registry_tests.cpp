#include <gtest/gtest.h>

#include "ck/options.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{

std::filesystem::path makeTempFilePath()
{
    auto base = std::filesystem::temp_directory_path();
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        auto candidate = base / ("ck_options_test_" + std::to_string(dist(rng)) + ".json");
        if (!std::filesystem::exists(candidate))
            return candidate;
    }
    return base / "ck_options_test.json";
}

} // namespace

TEST(OptionRegistry, RegistersAndReadsDefaults)
{
    ck::config::OptionRegistry registry("test-app");
    ck::config::OptionDefinition def{"featureEnabled", ck::config::OptionKind::Boolean, ck::config::OptionValue(true),
                                      "Feature Enabled", "Enables a feature for testing."};
    registry.registerOption(def);

    EXPECT_TRUE(registry.hasOption("featureEnabled"));
    EXPECT_TRUE(registry.getBool("featureEnabled"));

    registry.reset("featureEnabled");
    EXPECT_TRUE(registry.getBool("featureEnabled"));
}

TEST(OptionRegistry, NormalizesValuesToDefinitionTypes)
{
    ck::config::OptionRegistry registry("test-app");
    registry.registerOption({"threshold", ck::config::OptionKind::Integer, ck::config::OptionValue(std::int64_t{10}),
                             "Threshold", "Integer threshold"});
    registry.registerOption({"ignored", ck::config::OptionKind::Boolean, ck::config::OptionValue(false),
                             "Ignored", "Boolean flag"});

    registry.set("threshold", ck::config::OptionValue(std::string("42")));
    registry.set("ignored", ck::config::OptionValue(std::string("yes")));

    EXPECT_EQ(registry.getInteger("threshold"), 42);
    EXPECT_TRUE(registry.getBool("ignored"));
}

TEST(OptionRegistry, RestoresOverrideSnapshots)
{
    ck::config::OptionRegistry registry("test-app");
    registry.registerOption({"featureEnabled", ck::config::OptionKind::Boolean, ck::config::OptionValue(false),
                             "Feature Enabled", "Enables a feature for testing."});

    registry.set("featureEnabled", ck::config::OptionValue(true));
    auto before_change = registry.snapshot();
    registry.reset("featureEnabled");
    EXPECT_FALSE(registry.getBool("featureEnabled"));

    registry.restore(std::move(before_change));
    EXPECT_TRUE(registry.getBool("featureEnabled"));
}

TEST(OptionRegistry, PersistsValuesToDisk)
{
    ck::config::OptionRegistry registry("test-app");
    registry.registerOption({"paths", ck::config::OptionKind::StringList,
                             ck::config::OptionValue(std::vector<std::string>{}),
                             "Paths", "List of paths"});

    std::vector<std::string> expected{"/tmp/a", "/tmp/b"};
    registry.set("paths", ck::config::OptionValue(expected));

    const auto filePath = makeTempFilePath();
    ASSERT_TRUE(registry.saveToFile(filePath));

    ck::config::OptionRegistry loaded("test-app");
    loaded.registerOption({"paths", ck::config::OptionKind::StringList,
                           ck::config::OptionValue(std::vector<std::string>{}),
                           "Paths", "List of paths"});
    ASSERT_TRUE(loaded.loadFromFile(filePath));

    EXPECT_EQ(loaded.getStringList("paths"), expected);

    std::error_code ec;
    std::filesystem::remove(filePath, ec);
}

TEST(OptionRegistry, WritesVersionedProfilesWithoutBreakingFlatValueReads)
{
    ck::config::OptionRegistry registry("test-app");
    registry.registerOption({"threshold", ck::config::OptionKind::Integer, ck::config::OptionValue(std::int64_t{1}),
                             "Threshold", "Integer threshold"});
    registry.set("threshold", ck::config::OptionValue(std::int64_t{42}));

    const auto filePath = makeTempFilePath();
    ASSERT_TRUE(registry.saveToFile(filePath));

    std::ifstream saved(filePath);
    const std::string document((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    EXPECT_NE(document.find("\"_ck_options_format\": 1"), std::string::npos);
    EXPECT_NE(document.find("\"threshold\": 42"), std::string::npos);

    ck::config::OptionRegistry loaded("test-app");
    loaded.registerOption({"threshold", ck::config::OptionKind::Integer, ck::config::OptionValue(std::int64_t{1}),
                           "Threshold", "Integer threshold"});
    ASSERT_TRUE(loaded.loadFromFile(filePath));
    EXPECT_EQ(loaded.getInteger("threshold"), 42);

    std::error_code ec;
    std::filesystem::remove(filePath, ec);
}

TEST(OptionRegistry, RejectsCorruptProfilesWithoutMutatingKnownOrFutureValues)
{
    ck::config::OptionRegistry registry("test-app");
    registry.registerOption({"enabled", ck::config::OptionKind::Boolean, ck::config::OptionValue(false),
                             "Enabled", "Boolean flag"});
    registry.set("enabled", ck::config::OptionValue(true));

    const auto corruptPath = makeTempFilePath();
    {
        std::ofstream corrupt(corruptPath);
        corrupt << R"({"_ck_options_format":1,"enabled":{"not":"a boolean"},"future-option":"keep"})";
    }
    EXPECT_FALSE(registry.loadFromFile(corruptPath));
    EXPECT_TRUE(registry.getBool("enabled"));

    const auto futurePath = makeTempFilePath();
    {
        std::ofstream future(futurePath);
        future << R"({"_ck_options_format":9,"enabled":false,"future-option":{"nested":true}})";
    }
    ASSERT_TRUE(registry.loadFromFile(futurePath));
    EXPECT_FALSE(registry.getBool("enabled"));

    const auto roundTripPath = makeTempFilePath();
    ASSERT_TRUE(registry.saveToFile(roundTripPath));
    std::ifstream roundTrip(roundTripPath);
    const std::string saved((std::istreambuf_iterator<char>(roundTrip)), std::istreambuf_iterator<char>());
    EXPECT_NE(saved.find("\"future-option\""), std::string::npos);
    EXPECT_NE(saved.find("\"nested\": true"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(corruptPath, ec);
    std::filesystem::remove(futurePath, ec);
    std::filesystem::remove(roundTripPath, ec);
}
