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
    std::size_t context_window_tokens = 4096;
    std::size_t summary_trigger_tokens = 2048;
    int gpu_layers =
#if defined(__APPLE__)
        -1;
#else
        0;
#endif
};
} // namespace ck::ai
