#pragma once

#include "ck/ai/runtime_config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace ck::ai
{
struct Config
{
    RuntimeConfig runtime;
    struct ModelOverride
    {
        int gpu_layers = -9999; // sentinel meaning inherit
        std::size_t context_window_tokens = 0;
        std::size_t max_output_tokens = 0;
        std::size_t summary_trigger_tokens = 0;
    };
    std::unordered_map<std::string, ModelOverride> model_overrides;
};

class ConfigLoader
{
public:
    static std::filesystem::path default_config_path();
    static Config load_from_file(const std::filesystem::path &path);
    static Config load_or_default();
    static void save(const Config &config);
};
} // namespace ck::ai
