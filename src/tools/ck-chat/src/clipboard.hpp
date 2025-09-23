#pragma once
#include <string>

namespace clipboard
{
    std::string statusMessage();
    void copyToClipboard(const std::string &text);
}
