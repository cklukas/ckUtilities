#include "markdown_profile.hpp"

#include <string>
#include <string_view>

#include "ck/edit/markdown_parser.hpp"

namespace ck::vision
{
namespace
{

ckv::widgets::SyntaxTokenKind token_for(ck::edit::MarkdownSpanKind kind)
{
    switch (kind)
    {
    case ck::edit::MarkdownSpanKind::Bold:
    case ck::edit::MarkdownSpanKind::BoldItalic: return ckv::widgets::SyntaxTokenKind::Keyword;
    case ck::edit::MarkdownSpanKind::Italic: return ckv::widgets::SyntaxTokenKind::Type;
    case ck::edit::MarkdownSpanKind::Strikethrough: return ckv::widgets::SyntaxTokenKind::Comment;
    case ck::edit::MarkdownSpanKind::Code: return ckv::widgets::SyntaxTokenKind::String;
    case ck::edit::MarkdownSpanKind::Link:
    case ck::edit::MarkdownSpanKind::Image: return ckv::widgets::SyntaxTokenKind::Property;
    case ck::edit::MarkdownSpanKind::InlineHtml: return ckv::widgets::SyntaxTokenKind::Operator;
    case ck::edit::MarkdownSpanKind::PlainText: break;
    }
    return ckv::widgets::SyntaxTokenKind::Plain;
}

std::string state_from(const ck::edit::MarkdownParserState &state)
{
    return state.inFence ? "fence:" + state.fenceMarker : std::string{};
}

ck::edit::MarkdownParserState state_to(std::string_view serialized)
{
    ck::edit::MarkdownParserState state;
    constexpr std::string_view prefix = "fence:";
    if (serialized.starts_with(prefix))
    {
        state.inFence = true;
        state.fenceMarker = std::string(serialized.substr(prefix.size()));
    }
    return state;
}

} // namespace

bool register_markdown_syntax_profile(ckv::widgets::SyntaxProfileRegistry &registry)
{
    return registry.register_profile({
        .id = "markdown",
        .display_name = "Markdown",
        .detect = [](const ckv::widgets::LanguageDetectionInput &input) {
            if (input.file_name.ends_with(".md") || input.file_name.ends_with(".markdown") ||
                input.file_name.ends_with(".mdown"))
                return ckv::widgets::LanguageDetection{90, "Markdown file suffix"};
            if (input.content_prefix.starts_with("# ") || input.content_prefix.starts_with("## "))
                return ckv::widgets::LanguageDetection{35, "Markdown heading"};
            return ckv::widgets::LanguageDetection{};
        },
        .highlight_line = [](std::string_view line, std::string_view incoming_state) {
            ck::edit::MarkdownAnalyzer analyzer;
            ck::edit::MarkdownParserState state = state_to(incoming_state);
            const auto analysis = analyzer.analyzeLine(std::string(line), state);
            ckv::widgets::SyntaxLineResult result;
            result.next_state = state_from(state);

            const auto entire_line = [&] (ckv::widgets::SyntaxTokenKind kind) {
                if (!line.empty())
                    result.spans.push_back({0, line.size(), kind});
            };
            switch (analysis.kind)
            {
            case ck::edit::MarkdownLineKind::Heading: entire_line(ckv::widgets::SyntaxTokenKind::Keyword); break;
            case ck::edit::MarkdownLineKind::BlockQuote:
            case ck::edit::MarkdownLineKind::BulletListItem:
            case ck::edit::MarkdownLineKind::OrderedListItem:
            case ck::edit::MarkdownLineKind::TaskListItem:
            case ck::edit::MarkdownLineKind::TableSeparator:
            case ck::edit::MarkdownLineKind::HorizontalRule: entire_line(ckv::widgets::SyntaxTokenKind::Operator); break;
            case ck::edit::MarkdownLineKind::CodeFenceStart:
            case ck::edit::MarkdownLineKind::CodeFenceEnd:
            case ck::edit::MarkdownLineKind::FencedCode:
            case ck::edit::MarkdownLineKind::IndentedCode: entire_line(ckv::widgets::SyntaxTokenKind::String); break;
            case ck::edit::MarkdownLineKind::Html: entire_line(ckv::widgets::SyntaxTokenKind::Comment); break;
            default: break;
            }
            for (const auto &span : analysis.spans)
            {
                if (span.start < span.end && span.end <= line.size())
                    result.spans.push_back({span.start, span.end, token_for(span.kind)});
            }
            return result;
        },
    });
}

} // namespace ck::vision
