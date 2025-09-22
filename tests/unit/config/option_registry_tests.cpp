#include <gtest/gtest.h>

#include "ck/options.hpp"

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
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
