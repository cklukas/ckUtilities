// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// glob_match: the portable pattern matcher shared by every platform, from the
// POSIX shell pattern-matching notation (XCU 2.13.1), so ignore masks mean
// the same thing on macOS, Linux and Windows.
#include "disk_usage_platform.hpp"

namespace ck::du::platform
{

namespace
{

// Matches the bracket expression starting after '[' at pattern[p]. On
// success sets `end` to the index after ']' and `matched` to whether `ch` is in
// the set. A '[' without a closing ']' is an ordinary character.
bool match_bracket(const std::string &pattern, std::size_t p, char ch, std::size_t &end, bool &matched)
{
    bool negate = false;
    if (p < pattern.size() && (pattern[p] == '!' || pattern[p] == '^'))
    {
        negate = true;
        ++p;
    }
    bool found = false;
    bool first = true;
    while (p < pattern.size() && (first || pattern[p] != ']'))
    {
        first = false;
        char low = pattern[p];
        if (low == '\\' && p + 1 < pattern.size())
            low = pattern[++p];
        ++p;
        char high = low;
        if (p + 1 < pattern.size() && pattern[p] == '-' && pattern[p + 1] != ']')
        {
            high = pattern[p + 1];
            if (high == '\\' && p + 2 < pattern.size())
            {
                high = pattern[p + 2];
                ++p;
            }
            p += 2;
        }
        if (static_cast<unsigned char>(low) <= static_cast<unsigned char>(ch) &&
            static_cast<unsigned char>(ch) <= static_cast<unsigned char>(high))
            found = true;
    }
    if (p >= pattern.size())
        return false;
    end = p + 1;
    matched = found != negate;
    return true;
}

} // namespace

bool glob_match(const std::string &pattern, const std::string &value)
{
    std::size_t p = 0;
    std::size_t s = 0;
    // The last '*' seen, for backtracking: where in the pattern and the value.
    std::size_t star_p = std::string::npos;
    std::size_t star_s = 0;
    while (s < value.size())
    {
        if (p < pattern.size())
        {
            const char c = pattern[p];
            if (c == '*')
            {
                star_p = ++p;
                star_s = s;
                continue;
            }
            if (c == '?')
            {
                ++p;
                ++s;
                continue;
            }
            if (c == '[')
            {
                std::size_t end = 0;
                bool matched = false;
                if (match_bracket(pattern, p + 1, value[s], end, matched))
                {
                    if (matched)
                    {
                        p = end;
                        ++s;
                        continue;
                    }
                }
                else if (value[s] == '[')
                {
                    ++p;
                    ++s;
                    continue;
                }
            }
            else
            {
                char literal = c;
                std::size_t next = p + 1;
                if (c == '\\' && p + 1 < pattern.size())
                {
                    literal = pattern[p + 1];
                    next = p + 2;
                }
                if (literal == value[s])
                {
                    p = next;
                    ++s;
                    continue;
                }
            }
        }
        if (star_p == std::string::npos)
            return false;
        p = star_p;
        s = ++star_s;
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

} // namespace ck::du::platform
