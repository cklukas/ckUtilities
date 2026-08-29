#pragma once

#include <cvision/widgets/syntax_profile.hpp>

namespace ck::vision
{

// Registers the suite-owned Markdown syntax grammar.  It is intentionally an
// application profile until its grammar and acceptance are useful independently
// of ckUtilities' Markdown operations.
bool register_markdown_syntax_profile(ckv::widgets::SyntaxProfileRegistry &registry);

} // namespace ck::vision
