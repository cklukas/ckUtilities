#include "ck/ai/config.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string_view>

#include <gtest/gtest.h>

namespace
{
std::filesystem::path write_temp_config(std::string_view contents)
{
    auto temp = std::filesystem::temp_directory_path() /
                 std::filesystem::path("ckai-config-" +
                                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                      ".toml");
    std::ofstream file(temp);
    file << contents;
    return temp;
}
}

TEST(ConfigLoaderTests, ReturnsDefaultsWhenFileMissing)
{
    auto config = ck::ai::ConfigLoader::load_from_file("/nonexistent/ckai.toml");
    EXPECT_EQ(config.runtime.model_path, "");
    EXPECT_EQ(config.runtime.threads, 0);
    EXPECT_EQ(config.runtime.max_output_tokens, 512u);
}

TEST(ConfigLoaderTests, ParsesKnownKeys)
{
    auto path = write_temp_config(R"( [llm]
model = "test-model.gguf"
threads = 4

[limits]
max_output_tokens = 1024
)");

    auto config = ck::ai::ConfigLoader::load_from_file(path);
    EXPECT_EQ(config.runtime.model_path, "test-model.gguf");
    EXPECT_EQ(config.runtime.threads, 4);
    EXPECT_EQ(config.runtime.max_output_tokens, 1024u);
}
