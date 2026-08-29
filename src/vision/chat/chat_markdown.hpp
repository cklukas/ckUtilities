#pragma once

#include <string_view>

#include <cvision/widgets/flow_view.hpp>

namespace ck::vision
{

// Appends Markdown-aware flow inlines using the suite's framework-independent
// analyzer.  FlowView remains responsible only for layout, styling, and link
// activation; no Markdown parsing is duplicated in the chat presentation.
void append_markdown_flow(ckv::widgets::FlowBlock &block, std::string_view markdown, ckv::Attr base_attrs = static_cast<ckv::Attr>(0));

} // namespace ck::vision
