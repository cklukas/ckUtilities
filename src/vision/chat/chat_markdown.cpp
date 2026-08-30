#include "chat_markdown.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "ck/edit/markdown_parser.hpp"

namespace ck::vision
{
namespace
{

ckv::Attr attributes_for(const ck::edit::MarkdownLineInfo &line,
                         std::size_t start,
                         std::size_t end,
                         ckv::Attr base,
                         ChatMarkdownOptions options)
{
    ckv::Attr attributes = base;
    if (line.kind == ck::edit::MarkdownLineKind::Heading)
        attributes |= ckv::Attr::Bold;
    if (line.kind == ck::edit::MarkdownLineKind::CodeFenceStart ||
        line.kind == ck::edit::MarkdownLineKind::CodeFenceEnd ||
        line.kind == ck::edit::MarkdownLineKind::FencedCode ||
        line.kind == ck::edit::MarkdownLineKind::IndentedCode)
        attributes |= ckv::Attr::Dim;

    for (const auto &span : line.spans)
    {
        if (span.start > start || span.end < end)
            continue;
        switch (span.kind)
        {
        case ck::edit::MarkdownSpanKind::Bold: attributes |= ckv::Attr::Bold; break;
        case ck::edit::MarkdownSpanKind::Italic: attributes |= ckv::Attr::Italic; break;
        case ck::edit::MarkdownSpanKind::BoldItalic: attributes |= ckv::Attr::Bold | ckv::Attr::Italic; break;
        case ck::edit::MarkdownSpanKind::Strikethrough: attributes |= ckv::Attr::Strike; break;
        case ck::edit::MarkdownSpanKind::Code:
        case ck::edit::MarkdownSpanKind::InlineHtml: attributes |= ckv::Attr::Dim; break;
        case ck::edit::MarkdownSpanKind::Link:
        case ck::edit::MarkdownSpanKind::Image:
            if (options.render_links)
                attributes |= ckv::Attr::Underline;
            break;
        case ck::edit::MarkdownSpanKind::PlainText: break;
        }
    }
    return attributes;
}

std::optional<std::string> link_for(const ck::edit::MarkdownLineInfo &line,
                                    std::size_t start,
                                    std::size_t end,
                                    ChatMarkdownOptions options)
{
    if (!options.render_links)
        return std::nullopt;
    for (const auto &span : line.spans)
    {
        if ((span.kind == ck::edit::MarkdownSpanKind::Link || span.kind == ck::edit::MarkdownSpanKind::Image) &&
            span.start <= start && span.end >= end && !span.attribute.empty())
            return span.attribute;
    }
    return std::nullopt;
}

void append_line(ckv::widgets::FlowBlock &block,
                 const std::string &line,
                 const ck::edit::MarkdownLineInfo &analysis,
                 ckv::Attr base_attrs,
                 ChatMarkdownOptions options)
{
    std::vector<std::size_t> boundaries{0, line.size()};
    for (const auto &span : analysis.spans)
    {
        boundaries.push_back(std::min(span.start, line.size()));
        boundaries.push_back(std::min(span.end, line.size()));
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (std::size_t index = 1; index < boundaries.size(); ++index)
    {
        const std::size_t start = boundaries[index - 1];
        const std::size_t end = boundaries[index];
        if (start == end)
            continue;
        block.content.emplace_back(ckv::widgets::FlowText{line.substr(start, end - start),
                                                            attributes_for(analysis, start, end, base_attrs, options),
                                                            link_for(analysis, start, end, options)});
    }
}

} // namespace

void append_markdown_flow(ckv::widgets::FlowBlock &block,
                          std::string_view markdown,
                          ckv::Attr base_attrs,
                          ChatMarkdownOptions options)
{
    ck::edit::MarkdownAnalyzer analyzer;
    ck::edit::MarkdownParserState state;
    std::size_t start = 0;
    while (start <= markdown.size())
    {
        const std::size_t newline = markdown.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? markdown.size() : newline;
        const std::string line(markdown.substr(start, end - start));
        append_line(block, line, analyzer.analyzeLine(line, state), base_attrs, options);
        if (newline == std::string_view::npos)
            break;
        block.content.emplace_back(ckv::widgets::FlowLineBreak{});
        start = newline + 1;
    }
}

} // namespace ck::vision
