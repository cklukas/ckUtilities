#include "ck/ai/config.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <optional>

namespace ck::ai
{
namespace
{
std::string trim(std::string_view view)
{
    std::size_t start = 0;
    std::size_t end = view.size();
    while (start < end && std::isspace(static_cast<unsigned char>(view[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(view[end - 1])))
        --end;
    return std::string(view.substr(start, end - start));
}

void parse_assignment(std::string_view line, std::string &key, std::string &value)
{
    auto equal = line.find('=');
    if (equal == std::string_view::npos)
        return;
    key = trim(line.substr(0, equal));
    value = trim(line.substr(equal + 1));
}

bool is_section_header(std::string_view line, std::string &section)
{
    if (line.size() < 3 || line.front() != '[' || line.back() != ']')
        return false;
    section = trim(line.substr(1, line.size() - 2));
    return true;
}

std::string parse_string(std::string value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

std::optional<long long> parse_integer(const std::string &value)
{
    long long result = 0;
    auto begin = value.data();
    auto end = value.data() + value.size();
    auto rc = std::from_chars(begin, end, result);
    if (rc.ec == std::errc())
        return result;
    return std::nullopt;
}
} // namespace

std::filesystem::path ConfigLoader::default_config_path()
{
    std::filesystem::path config_home;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        config_home = xdg;
    else if (const char *home = std::getenv("HOME"); home && *home)
        config_home = std::filesystem::path(home) / ".config";
    else
        config_home = std::filesystem::current_path();

    return config_home / "cktools" / "ckai.toml";
}

Config ConfigLoader::load_from_file(const std::filesystem::path &path)
{
    Config config;
    config.runtime.max_output_tokens = 512;
    config.runtime.context_window_tokens = 4096;
    config.runtime.summary_trigger_tokens = 2048;
#if defined(__APPLE__)
    config.runtime.gpu_layers = -1;
#else
    config.runtime.gpu_layers = 0;
#endif
    config.runtime.threads = 0;
    config.model_overrides.clear();

    std::ifstream stream(path);
    if (!stream)
        return config;

    std::string line;
    std::string section;
    while (std::getline(stream, line))
    {
        auto hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;

        std::string maybe_section;
        if (is_section_header(line, maybe_section))
        {
            section = maybe_section;
            continue;
        }

        std::string key;
        std::string value;
        parse_assignment(line, key, value);
        if (key.empty())
            continue;

        if (section == "llm")
        {
            if (key == "model")
            {
                config.runtime.model_path = parse_string(value);
            }
            else if (key == "threads")
            {
                if (auto parsed = parse_integer(value))
                    config.runtime.threads = static_cast<int>(*parsed);
            }
            else if (key == "gpu_layers")
            {
                if (auto parsed = parse_integer(value))
                    config.runtime.gpu_layers = static_cast<int>(*parsed);
            }
        }
        else if (section == "limits")
        {
            if (key == "max_output_tokens")
            {
                if (auto parsed = parse_integer(value))
                    config.runtime.max_output_tokens = static_cast<std::size_t>(*parsed);
            }
            if (key == "context_window_tokens")
            {
                if (auto parsed = parse_integer(value))
                    config.runtime.context_window_tokens = static_cast<std::size_t>(*parsed);
            }
            if (key == "summary_trigger_tokens")
            {
                if (auto parsed = parse_integer(value))
                    config.runtime.summary_trigger_tokens = static_cast<std::size_t>(*parsed);
            }
        }
        else if (section.rfind("model.", 0) == 0)
        {
            std::string modelId = section.substr(6);
            if (modelId.empty())
                continue;
            auto &overrideConfig = config.model_overrides[modelId];
            if (overrideConfig.gpu_layers == -9999)
                overrideConfig.gpu_layers = -1;
            if (key == "gpu_layers")
            {
                if (auto parsed = parse_integer(value))
                    overrideConfig.gpu_layers = static_cast<int>(*parsed);
            }
        }
    }

    return config;
}

Config ConfigLoader::load_or_default()
{
    return load_from_file(default_config_path());
}

void ConfigLoader::save(const Config &config)
{
    auto path = default_config_path();
    std::filesystem::create_directories(path.parent_path());

    std::ofstream out(path);
    if (!out.is_open())
        return;

    out << "[llm]\n";
    if (!config.runtime.model_path.empty())
        out << "model = \"" << config.runtime.model_path << "\"\n";
    out << "threads = " << config.runtime.threads << "\n";
    out << "gpu_layers = " << config.runtime.gpu_layers << "\n";

    out << "\n[limits]\n";
    out << "max_output_tokens = " << config.runtime.max_output_tokens << "\n";
    out << "context_window_tokens = " << config.runtime.context_window_tokens << "\n";
    out << "summary_trigger_tokens = " << config.runtime.summary_trigger_tokens << "\n";

    for (const auto &entry : config.model_overrides)
    {
        const auto &overrideConfig = entry.second;
        if (overrideConfig.gpu_layers == -9999)
            continue;
        out << "\n[model." << entry.first << "]\n";
        out << "gpu_layers = " << overrideConfig.gpu_layers << "\n";
    }
}

} // namespace ck::ai
