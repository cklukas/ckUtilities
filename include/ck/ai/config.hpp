#pragma once

#include "ck/ai/runtime_config.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace ck::ai
{
struct Config
{
    RuntimeConfig runtime;
};

class ConfigLoader
{
public:
    static std::filesystem::path default_config_path();
    static Config load_from_file(const std::filesystem::path &path);
    static Config load_or_default();
};
} // namespace ck::ai
