#pragma once

#include <string>
#include <string_view>

namespace ck::vision
{

// A deliberately conservative, deterministic Markdown cleanup pass. It does
// not reinterpret prose or code; it only removes accidental single trailing
// spaces, preserves Markdown's two-space hard-break marker, and leaves one
// final newline for ordinary text files.
std::string normalise_markdown_whitespace(std::string_view text);

} // namespace ck::vision
