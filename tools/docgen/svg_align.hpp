// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace ck::docgen {

// Snaps multi-column SVG <text> runs back onto the exact cell grid.
//
// ckVision's render_svg_snapshot merges consecutive cells with identical style
// across intervening spaces into a single <text> element. When a viewer's
// monospace font has an advance slightly wider than cell_width_px (for example
// SF Mono / ui-monospace at 15px on macOS, ~9.21px per 9px cell), long runs
// drift to the right: right-aligned items such as the top-right clock clip at
// the right edge, and unselected rows drift relative to selected rows whose
// highlight starts a fresh <text> run at the exact column coordinate.
//
// Splitting <text> runs on gaps of two or more spaces restores exact cell-grid
// x-coordinates for every column and right-aligned item, and nudging the 15px
// stylesheet font size to 14.5px keeps single-word and prose runs within 9px
// per cell on macOS, Linux, and Windows.
inline std::string align_svg_text_runs(std::string svg, int cell_width_px = 9) {
    // Keep glyph advance <= 9px even on wider monospace faces (e.g. SF Mono).
    const std::string_view old_size = ";font-size:15px}</style>";
    const std::string_view new_size = ";font-size:14.5px}</style>";
    if (const auto pos = svg.find(old_size); pos != std::string::npos) {
        svg.replace(pos, old_size.size(), new_size);
    }

    auto advance_entity_or_utf8 = [](std::string_view s, std::size_t pos) -> std::size_t {
        if (s[pos] == '&') {
            const auto semi = s.find(';', pos + 1);
            if (semi != std::string_view::npos && semi - pos <= 10)
                return semi + 1;
        }
        const unsigned char ch = static_cast<unsigned char>(s[pos]);
        std::size_t next = pos + 1;
        if ((ch & 0x80u) != 0) {
            while (next < s.size() && (static_cast<unsigned char>(s[next]) & 0xC0u) == 0x80u)
                ++next;
        }
        return next;
    };

    std::string out;
    out.reserve(svg.size() + svg.size() / 8);

    std::size_t line_start = 0;
    while (line_start < svg.size()) {
        const auto nl = svg.find('\n', line_start);
        const std::size_t line_end = (nl == std::string::npos) ? svg.size() : nl;
        const std::string_view line(svg.data() + line_start, line_end - line_start);
        const bool has_nl = (nl != std::string::npos);
        line_start = has_nl ? nl + 1 : svg.size();

        const auto text_open = line.find("<text x=\"");
        const auto text_close = line.rfind("</text>");
        if (text_open == std::string_view::npos || text_close == std::string_view::npos ||
            text_close <= text_open) {
            out.append(line);
            if (has_nl) out.push_back('\n');
            continue;
        }

        const std::size_t x_val_start = text_open + 9; // after `<text x="`
        const auto x_val_end = line.find('"', x_val_start);
        const auto tag_end = line.find('>', x_val_end);
        if (x_val_end == std::string_view::npos || tag_end == std::string_view::npos ||
            tag_end >= text_close) {
            out.append(line);
            if (has_nl) out.push_back('\n');
            continue;
        }

        int base_x = 0;
        const auto parsed = std::from_chars(line.data() + x_val_start, line.data() + x_val_end, base_x);
        if (parsed.ec != std::errc{} || parsed.ptr != line.data() + x_val_end) {
            out.append(line);
            if (has_nl) out.push_back('\n');
            continue;
        }

        const std::string_view content = line.substr(tag_end + 1, text_close - (tag_end + 1));
        if (content.find("  ") == std::string_view::npos) {
            out.append(line);
            if (has_nl) out.push_back('\n');
            continue;
        }

        const std::string_view prefix = line.substr(0, text_open);
        const std::string_view attrs = line.substr(x_val_end + 1, tag_end - (x_val_end + 1));
        const std::string_view suffix = line.substr(text_close + 7);

        std::size_t pos = 0;
        int cell = 0;
        while (pos < content.size()) {
            // Skip leading spaces between segments.
            while (pos < content.size() && content[pos] == ' ') {
                ++pos;
                ++cell;
            }
            if (pos >= content.size()) break;

            const std::size_t seg_start = pos;
            const int seg_cell = cell;
            while (pos < content.size()) {
                if (content[pos] == ' ' && pos + 1 < content.size() && content[pos + 1] == ' ')
                    break;
                pos = advance_entity_or_utf8(content, pos);
                ++cell;
            }
            std::size_t seg_end = pos;
            while (seg_end > seg_start && content[seg_end - 1] == ' ')
                --seg_end;
            if (seg_end > seg_start) {
                out.append(prefix);
                out.append("<text x=\"");
                out.append(std::to_string(base_x + seg_cell * cell_width_px));
                out.push_back('"');
                out.append(attrs);
                out.push_back('>');
                out.append(content.substr(seg_start, seg_end - seg_start));
                out.append("</text>");
                out.append(suffix);
                if (has_nl) out.push_back('\n');
            }
        }
    }
    return out;
}

} // namespace ck::docgen
