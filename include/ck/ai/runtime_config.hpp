#pragma once

#include <cstddef>
#include <string>

namespace ck::ai
{
struct RuntimeConfig
{
    std::string model_path;
    int threads = 0;
    std::size_t max_output_tokens = 512;
};
} // namespace ck::ai
