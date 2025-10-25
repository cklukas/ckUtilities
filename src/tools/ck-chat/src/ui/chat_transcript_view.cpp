#include "chat_transcript_view.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <iostream>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ck/edit/markdown_parser.hpp"
#include "../tvision_include.hpp"

namespace
{
  // Style constants for text attributes
  const ushort sfBold = 0x0800;
  const ushort sfItalic = 0x0080;
  const ushort sfUnderline = 0x0100;

  using StyleMask = ChatTranscriptView::StyleMask;

  constexpr StyleMask kStyleNone = 0;
  constexpr StyleMask kStyleBold = 1u << 0;
  constexpr StyleMask kStyleItalic = 1u << 1;
  constexpr StyleMask kStyleStrikethrough = 1u << 2;
  constexpr StyleMask kStyleInlineCode = 1u << 3;
  constexpr StyleMask kStyleLink = 1u << 4;
  constexpr StyleMask kStyleHeading = 1u << 5;
  constexpr unsigned kStyleHeadingLevelShift = 16;
  constexpr StyleMask kStyleHeadingLevelMask = 7u << kStyleHeadingLevelShift;  // Mask to extract heading level
  constexpr StyleMask kStyleHeading1 = kStyleHeading | (1u << kStyleHeadingLevelShift);  // Combined with heading
  constexpr StyleMask kStyleHeading2 = kStyleHeading | (2u << kStyleHeadingLevelShift);  // Combined with heading
  constexpr StyleMask kStyleHeading3 = kStyleHeading | (3u << kStyleHeadingLevelShift);  // Combined with heading
  constexpr StyleMask kStyleHeading4 = kStyleHeading | (4u << kStyleHeadingLevelShift);  // Combined with heading
  constexpr StyleMask kStyleHeading5 = kStyleHeading | (5u << kStyleHeadingLevelShift);  // Combined with heading
  constexpr StyleMask kStyleHeading6 = kStyleHeading | (6u << kStyleHeadingLevelShift);  // Combined with heading
  constexpr StyleMask kStyleQuote = 1u << 6;
  constexpr StyleMask kStyleListMarker = 1u << 7;
  constexpr StyleMask kStyleTableBorder = 1u << 8;
  constexpr StyleMask kStyleTableHeader = 1u << 9;
  constexpr StyleMask kStyleTableCell = 1u << 10;
  constexpr StyleMask kStyleCodeBlock = 1u << 11;
  constexpr StyleMask kStylePrefix = 1u << 12;
  constexpr StyleMask kStyleHorizontalRule = 1u << 13;
  constexpr StyleMask kStyleUnderline = 1u << 14;

  struct LinkRange
  {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string url;
  };

  struct StyledLine
  {
    std::string text;
    std::vector<StyleMask> styles;
    std::vector<LinkRange> links;
  };

  struct ChannelSegment
  {
    std::string channel;
    std::string text;
    bool from_marker = false;
  };

  std::string_view trim_view(std::string_view view)
  {
    const char *whitespace = " \t\r\n";
    auto begin = view.find_first_not_of(whitespace);
    if (begin == std::string_view::npos)
      return std::string_view();
    auto end = view.find_last_not_of(whitespace);
    return view.substr(begin, end - begin + 1);
  }

  std::string trim_copy(std::string_view view)
  {
    std::string_view trimmed = trim_view(view);
    return std::string(trimmed);
  }

  std::string to_lower_copy(std::string_view view)
  {
    std::string lowered;
    lowered.reserve(view.size());
    for (char ch : view)
      lowered.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return lowered;
  }

  std::string normalize_html_line_breaks(const std::string &text)
  {
    std::string output;
    output.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i)
    {
      char ch = text[i];
      if (ch == '<')
      {
        std::size_t j = i + 1;
        while (j < text.size() &&
               std::isspace(static_cast<unsigned char>(text[j])))
          ++j;
        std::size_t tagStart = j;
        while (j < text.size() &&
               std::isalpha(static_cast<unsigned char>(text[j])))
          ++j;
        if (tagStart != j)
        {
          std::string tag = to_lower_copy(
              std::string_view(text.data() + tagStart, j - tagStart));
          if (tag == "br")
          {
            while (j < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[j])))
              ++j;
            if (j < text.size() && text[j] == '/')
            {
              ++j;
              while (j < text.size() &&
                     std::isspace(static_cast<unsigned char>(text[j])))
                ++j;
            }
            if (j < text.size() && text[j] == '>')
            {
              output.push_back('\n');
              i = j;
              continue;
            }
          }
        }
      }

      output.push_back(ch);
    }

    return output;
  }

  uint32_t next_codepoint(std::string_view text, std::size_t &index)
  {
    if (index >= text.size())
      return 0;
    unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80)
    {
      ++index;
      return lead;
    }
    std::size_t remaining = text.size() - index;
    if ((lead & 0xE0) == 0xC0 && remaining >= 2)
    {
      uint32_t cp = (lead & 0x1F) << 6;
      unsigned char b1 = static_cast<unsigned char>(text[index + 1]);
      if ((b1 & 0xC0) == 0x80)
      {
        cp |= (b1 & 0x3F);
        index += 2;
        return cp;
      }
    }
    else if ((lead & 0xF0) == 0xE0 && remaining >= 3)
    {
      unsigned char b1 = static_cast<unsigned char>(text[index + 1]);
      unsigned char b2 = static_cast<unsigned char>(text[index + 2]);
      if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80)
      {
        uint32_t cp = (lead & 0x0F) << 12;
        cp |= (b1 & 0x3F) << 6;
        cp |= (b2 & 0x3F);
        index += 3;
        return cp;
      }
    }
    else if ((lead & 0xF8) == 0xF0 && remaining >= 4)
    {
      unsigned char b1 = static_cast<unsigned char>(text[index + 1]);
      unsigned char b2 = static_cast<unsigned char>(text[index + 2]);
      unsigned char b3 = static_cast<unsigned char>(text[index + 3]);
      if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 &&
          (b3 & 0xC0) == 0x80)
      {
        uint32_t cp = (lead & 0x07) << 18;
        cp |= (b1 & 0x3F) << 12;
        cp |= (b2 & 0x3F) << 6;
        cp |= (b3 & 0x3F);
        index += 4;
        return cp;
      }
    }

    // Invalid sequence, consume one byte and replace with replacement char
    ++index;
    return lead;
  }

  struct Interval
  {
    uint32_t first;
    uint32_t last;
  };

  int bisearch(uint32_t codepoint, const Interval *table, int length)
  {
    int low = 0;
    int high = length - 1;
    if (codepoint < table[0].first || codepoint > table[high].last)
      return 0;
    while (low <= high)
    {
      int mid = (low + high) / 2;
      if (codepoint > table[mid].last)
        low = mid + 1;
      else if (codepoint < table[mid].first)
        high = mid - 1;
      else
        return 1;
    }
    return 0;
  }

  static const Interval kCombiningIntervals[] = {
      {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
      {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0600, 0x0605},
      {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DD},
      {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x070F, 0x070F},
      {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
      {0x07FD, 0x07FD}, {0x0816, 0x0819}, {0x081B, 0x0823}, {0x0825, 0x0827},
      {0x0829, 0x082D}, {0x0859, 0x085B}, {0x0898, 0x089F}, {0x08CA, 0x0903},
      {0x093A, 0x093C}, {0x093E, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963},
      {0x0981, 0x0983}, {0x09BC, 0x09BC}, {0x09BE, 0x09C4}, {0x09C7, 0x09C8},
      {0x09CB, 0x09CD}, {0x09D7, 0x09D7}, {0x09E2, 0x09E3}, {0x09FE, 0x09FE},
      {0x0A01, 0x0A03}, {0x0A3C, 0x0A3C}, {0x0A3E, 0x0A42}, {0x0A47, 0x0A48},
      {0x0A4B, 0x0A4D}, {0x0A51, 0x0A51}, {0x0A70, 0x0A71}, {0x0A75, 0x0A75},
      {0x0A81, 0x0A83}, {0x0ABC, 0x0ABC}, {0x0ABE, 0x0AC5}, {0x0AC7, 0x0AC9},
      {0x0ACB, 0x0ACD}, {0x0AE2, 0x0AE3}, {0x0AFA, 0x0AFF}, {0x0B01, 0x0B03},
      {0x0B3C, 0x0B3C}, {0x0B3E, 0x0B44}, {0x0B47, 0x0B48}, {0x0B4B, 0x0B4D},
      {0x0B56, 0x0B57}, {0x0B62, 0x0B63}, {0x0B82, 0x0B82}, {0x0BBE, 0x0BC2},
      {0x0BC6, 0x0BC8}, {0x0BCA, 0x0BCD}, {0x0BD7, 0x0BD7}, {0x0C00, 0x0C04},
      {0x0C3C, 0x0C3C}, {0x0C3E, 0x0C44}, {0x0C46, 0x0C48}, {0x0C4A, 0x0C4D},
      {0x0C55, 0x0C56}, {0x0C62, 0x0C63}, {0x0C81, 0x0C83}, {0x0CBC, 0x0CBC},
      {0x0CBE, 0x0CC4}, {0x0CC6, 0x0CC8}, {0x0CCA, 0x0CCD}, {0x0CD5, 0x0CD6},
      {0x0CE2, 0x0CE3}, {0x0CF3, 0x0CF3}, {0x0D00, 0x0D03}, {0x0D3B, 0x0D3C},
      {0x0D3E, 0x0D44}, {0x0D46, 0x0D48}, {0x0D4A, 0x0D4D}, {0x0D57, 0x0D57},
      {0x0D62, 0x0D63}, {0x0D81, 0x0D83}, {0x0DCA, 0x0DCA}, {0x0DCF, 0x0DD4},
      {0x0DD6, 0x0DD6}, {0x0DD8, 0x0DDF}, {0x0DF2, 0x0DF3}, {0x0E31, 0x0E31},
      {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
      {0x0EC8, 0x0ECD}, {0x0F18, 0x0F19}, {0x0F3E, 0x0F3F}, {0x0F71, 0x0F84},
      {0x0F86, 0x0F87}, {0x0F8D, 0x0F97}, {0x0F99, 0x0FBC}, {0x0FC6, 0x0FC6},
      {0x102B, 0x103E}, {0x1056, 0x1059}, {0x105E, 0x1060}, {0x1062, 0x1064},
      {0x1067, 0x106D}, {0x1071, 0x1074}, {0x1082, 0x108D}, {0x108F, 0x108F},
      {0x109A, 0x109D}, {0x135D, 0x135F}, {0x1712, 0x1715}, {0x1732, 0x1734},
      {0x1752, 0x1753}, {0x1772, 0x1773}, {0x17B4, 0x17D3}, {0x17DD, 0x17DD},
      {0x180B, 0x180F}, {0x1885, 0x1886}, {0x18A9, 0x18A9}, {0x1920, 0x192B},
      {0x1930, 0x193B}, {0x1A17, 0x1A1B}, {0x1A55, 0x1A7F}, {0x1AB0, 0x1ACE},
      {0x1B00, 0x1B04}, {0x1B34, 0x1B44}, {0x1B6B, 0x1B73}, {0x1B80, 0x1B82},
      {0x1BA1, 0x1BAD}, {0x1BE6, 0x1BF3}, {0x1C24, 0x1C37}, {0x1CD0, 0x1CD2},
      {0x1CD4, 0x1CE8}, {0x1CED, 0x1CED}, {0x1CF2, 0x1CF4}, {0x1CF7, 0x1CF9},
      {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
      {0x2066, 0x206F}, {0x20D0, 0x20F0}, {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F},
      {0x2DE0, 0x2DFF}, {0x302A, 0x302F}, {0x3099, 0x309A}, {0xA674, 0xA67D},
      {0xA69E, 0xA69F}, {0xA6F0, 0xA6F1}, {0xA802, 0xA802}, {0xA806, 0xA806},
      {0xA80B, 0xA80B}, {0xA823, 0xA827}, {0xA82C, 0xA82C}, {0xA880, 0xA881},
      {0xA8B4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA926, 0xA92D}, {0xA947, 0xA953},
      {0xA980, 0xA983}, {0xA9B3, 0xA9C0}, {0xA9E5, 0xA9E5}, {0xAA29, 0xAA36},
      {0xAA43, 0xAA43}, {0xAA4C, 0xAA4D}, {0xAA7B, 0xAA7D}, {0xAAB0, 0xAAB0},
      {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF}, {0xAAC1, 0xAAC1},
      {0xAAEB, 0xAAEF}, {0xAAF5, 0xAAF6}, {0xABE3, 0xABEA}, {0xABEC, 0xABED},
      {0xFB1E, 0xFB1E}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
      {0xFFF9, 0xFFFB}, {0x101FD, 0x101FD}, {0x102E0, 0x102E0},
      {0x10376, 0x1037A}, {0x10A01, 0x10A03}, {0x10A05, 0x10A06},
      {0x10A0C, 0x10A0F}, {0x10A38, 0x10A3A}, {0x10A3F, 0x10A3F},
      {0x10AE5, 0x10AE6}, {0x11000, 0x11002}, {0x11038, 0x11046},
      {0x1107F, 0x11082}, {0x110B0, 0x110BA}, {0x110C2, 0x110C2},
      {0x11100, 0x11103}, {0x11127, 0x11134}, {0x11145, 0x11146},
      {0x11173, 0x11173}, {0x11180, 0x11182}, {0x111B3, 0x111C0},
      {0x111C9, 0x111CC}, {0x111CE, 0x111CF}, {0x1122C, 0x11237},
      {0x1123E, 0x1123E}, {0x112DF, 0x112EA}, {0x11300, 0x11303},
      {0x1133B, 0x1133C}, {0x1133E, 0x11344}, {0x11347, 0x11348},
      {0x1134B, 0x1134D}, {0x11357, 0x11357}, {0x11362, 0x11363},
      {0x11366, 0x1136C}, {0x11370, 0x11374}, {0x11435, 0x11446},
      {0x1145E, 0x1145F}, {0x114B0, 0x114C3}, {0x115AF, 0x115B5},
      {0x115B8, 0x115C0}, {0x115DC, 0x115DD}, {0x11630, 0x11640},
      {0x116AB, 0x116B7}, {0x1171D, 0x1172B}, {0x1182C, 0x1183A},
      {0x11838, 0x1183A}, {0x11930, 0x1193F}, {0x11940, 0x11940},
      {0x11942, 0x11943}, {0x119D1, 0x119D7}, {0x119DA, 0x119E0},
      {0x119E4, 0x119E4}, {0x11A01, 0x11A0A}, {0x11A33, 0x11A3E},
      {0x11A47, 0x11A47}, {0x11A51, 0x11A5B}, {0x11A8A, 0x11A99},
      {0x11C2F, 0x11C3F}, {0x11C92, 0x11CA7}, {0x11CA9, 0x11CB6},
      {0x11D31, 0x11D45}, {0x11D47, 0x11D47}, {0x11D8A, 0x11D97},
      {0x11EF3, 0x11EF6}, {0x11F00, 0x11F01}, {0x11F36, 0x11F3A},
      {0x11F3E, 0x11F42}, {0x13430, 0x13438}, {0x13440, 0x13455},
      {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36}, {0x16F4F, 0x16F4F},
      {0x16F51, 0x16F87}, {0x16F8F, 0x16F92}, {0x16FE4, 0x16FE4},
      {0x16FF0, 0x16FF1}, {0x1BC9D, 0x1BC9E}, {0x1CF00, 0x1CF2D},
      {0x1CF30, 0x1CF46}, {0x1D165, 0x1D169}, {0x1D16D, 0x1D172},
      {0x1D17B, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD},
      {0x1D242, 0x1D244}, {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C},
      {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84}, {0x1DA9B, 0x1DA9F},
      {0x1DAA1, 0x1DAAF}, {0x1E000, 0x1E02A}, {0x1E130, 0x1E13D},
      {0x1E2AE, 0x1E2AE}, {0x1E2EC, 0x1E2EF}, {0x1E8D0, 0x1E8D6},
      {0x1E944, 0x1E94B}, {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF}};

  static const Interval kDoubleWidthIntervals[] = {
      {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
      {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
      {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
      {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
      {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
      {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
      {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
      {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
      {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x2E99},
      {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5}, {0x2FF0, 0x2FFB}, {0x3000, 0x303E},
      {0x3040, 0x3247}, {0x3250, 0x4DBF}, {0x4E00, 0xA4C6}, {0xA960, 0xA97C},
      {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE52},
      {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6},
      {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
      {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B},
      {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265},
      {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C},
      {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
      {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
      {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
      {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A},
      {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F},
      {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
      {0x1F6D5, 0x1F6D7}, {0x1F6DD, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
      {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0},
      {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF},
      {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA88}, {0x1FA90, 0x1FABD},
      {0x1FABF, 0x1FAC5}, {0x1FACE, 0x1FADB}, {0x1FAE0, 0x1FAE8},
      {0x1FAF0, 0x1FAF8}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD}};

  int mk_wcwidth(uint32_t codepoint)
  {
    if (codepoint == 0)
      return 0;
    if (codepoint < 0x20)
      return 0;
    if (codepoint >= 0x7F && codepoint < 0xA0)
      return 0;
    if (bisearch(codepoint, kCombiningIntervals,
                 static_cast<int>(std::size(kCombiningIntervals))))
      return 0;
    if (bisearch(codepoint, kDoubleWidthIntervals,
                 static_cast<int>(std::size(kDoubleWidthIntervals))))
      return 2;
    return 1;
  }


  int codepoint_display_width(uint32_t codepoint)
  {
    if (codepoint == '\n' || codepoint == '\r')
      return 0;
    int width = mk_wcwidth(codepoint);
    if (width < 0)
      return 0;
    return width;
  }

  int display_width(std::string_view text)
  {
    int width = 0;
    std::size_t index = 0;
    while (index < text.size())
    {
      uint32_t cp = next_codepoint(text, index);
      width += codepoint_display_width(cp);
    }
    return width;
  }

  int max_line_length(std::string_view text)
  {
    int maxLen = 0;
    int current = 0;
    std::size_t index = 0;
    while (index < text.size())
    {
      uint32_t cp = next_codepoint(text, index);
      if (cp == '\n')
      {
        maxLen = std::max(maxLen, current);
        current = 0;
      }
      else if (cp != '\r')
      {
        current += codepoint_display_width(cp);
      }
    }
    maxLen = std::max(maxLen, current);
    return maxLen;
  }

  constexpr std::string_view kBoxTopLeft = "\xE2\x94\x8C";
  constexpr std::string_view kBoxTopJoin = "\xE2\x94\xAC";
  constexpr std::string_view kBoxTopRight = "\xE2\x94\x90";
  constexpr std::string_view kBoxBottomLeft = "\xE2\x94\x94";
  constexpr std::string_view kBoxBottomJoin = "\xE2\x94\xB4";
  constexpr std::string_view kBoxBottomRight = "\xE2\x94\x98";
  constexpr std::string_view kBoxHorizontal = "\xE2\x94\x80";
  constexpr std::string_view kBoxVertical = "\xE2\x94\x82";
  constexpr std::string_view kBoxHeaderLeft = "\xE2\x95\x9E";
  constexpr std::string_view kBoxHeaderJoin = "\xE2\x95\xAA";
  constexpr std::string_view kBoxHeaderRight = "\xE2\x95\xA1";
  constexpr std::string_view kBoxHeaderHorizontal = "\xE2\x95\x90";

  bool table_debug_enabled()
  {
    static bool enabled = (std::getenv("CK_CHAT_DEBUG_TABLES") != nullptr);
    return enabled;
  }

  std::ofstream &debug_log_stream()
  {
    static std::ofstream log;
    static bool initialized = false;
    if (!initialized)
    {
      initialized = true;
      const char *path = std::getenv("CK_CHAT_DEBUG_LOG");
      if (!path || !*path)
        path = "debug.log";
      log.open(path, std::ios::app);
      if (!log)
        std::cerr << "[ck-chat] failed to open debug log at '" << path << "'\n";
    }
    return log;
  }

  void append_text_with_style(std::string &line,
                              std::vector<StyleMask> &styles,
                              std::string_view text, StyleMask style)
  {
    for (unsigned char ch : text)
    {
      line.push_back(static_cast<char>(ch));
      styles.push_back(style);
    }
  }

  void append_char_with_style(std::string &line,
                              std::vector<StyleMask> &styles, char ch,
                              StyleMask style)
  {
    append_text_with_style(line, styles, std::string_view(&ch, 1), style);
  }

  void append_repeat_char_with_style(std::string &line,
                                     std::vector<StyleMask> &styles,
                                     char ch, StyleMask style, int count)
  {
    for (int i = 0; i < count; ++i)
      append_char_with_style(line, styles, ch, style);
  }

  void append_repeat_text_with_style(std::string &line,
                                     std::vector<StyleMask> &styles,
                                     std::string_view text,
                                     StyleMask style, int count)
  {
    for (int i = 0; i < count; ++i)
      append_text_with_style(line, styles, text, style);
  }

  struct InlineContent
  {
    std::string text;
    std::vector<ck::edit::MarkdownSpan> spans;
    std::vector<LinkRange> links;
  };

  InlineContent sanitize_inline_markup(const std::string &text,
                                       const std::vector<ck::edit::MarkdownSpan> &spans)
  {
    InlineContent result;
    if (text.empty())
    {
      result.text = text;
      result.spans = spans;
      return result;
    }

    std::vector<bool> remove(text.size(), false);
    std::vector<std::pair<std::size_t, std::size_t>> underlineRanges;

    auto check_before = [&](std::size_t pos, char marker, std::size_t count)
    {
      if (count == 0)
        return true;
      if (pos < count)
        return false;
      for (std::size_t i = 0; i < count; ++i)
      {
        if (text[pos - 1 - i] != marker)
          return false;
      }
      return true;
    };

    auto check_after = [&](std::size_t pos, char marker, std::size_t count)
    {
      if (count == 0)
        return true;
      if (pos + count > text.size())
        return false;
      for (std::size_t i = 0; i < count; ++i)
      {
        if (text[pos + i] != marker)
          return false;
      }
      return true;
    };

    auto mark_before = [&](std::size_t pos, char marker, std::size_t count)
    {
      for (std::size_t i = 0; i < count; ++i)
        remove[pos - 1 - i] = true;
    };

    auto mark_after = [&](std::size_t pos, char marker, std::size_t count)
    {
      for (std::size_t i = 0; i < count; ++i)
        remove[pos + i] = true;
    };

    for (const auto &span : spans)
    {
      if (span.start >= span.end || span.end > text.size())
        continue;

      switch (span.kind)
      {
      case ck::edit::MarkdownSpanKind::Bold:
      case ck::edit::MarkdownSpanKind::Italic:
      case ck::edit::MarkdownSpanKind::BoldItalic:
      {
        if (span.start == 0 || span.end >= text.size())
          break;
        char marker = text[span.start - 1];
        char closingMarker = text[span.end];
        if ((marker != '*' && marker != '_') || marker != closingMarker)
          break;

        std::size_t needed = 0;
        if (span.kind == ck::edit::MarkdownSpanKind::Italic)
          needed = 1;
        else if (span.kind == ck::edit::MarkdownSpanKind::Bold)
          needed = 2;
        else
          needed = 3;

        if (check_before(span.start, marker, needed) &&
            check_after(span.end, marker, needed))
        {
          mark_before(span.start, marker, needed);
          mark_after(span.end, marker, needed);
        }
        break;
      }
      case ck::edit::MarkdownSpanKind::Strikethrough:
      {
        constexpr std::size_t needed = 2;
        if (span.start < needed || span.end + needed > text.size())
          break;
        if (check_before(span.start, '~', needed) &&
            check_after(span.end, '~', needed))
        {
          mark_before(span.start, '~', needed);
          mark_after(span.end, '~', needed);
        }
        break;
      }
      case ck::edit::MarkdownSpanKind::Code:
      {
        std::size_t before = 0;
        std::size_t pos = span.start;
        while (pos > 0 && text[pos - 1] == '`')
        {
          ++before;
          --pos;
        }
        std::size_t after = 0;
        pos = span.end;
        while (pos < text.size() && text[pos] == '`')
        {
          ++after;
          ++pos;
        }
        std::size_t fence = std::min(before, after);
        if (fence > 0)
        {
          if (check_before(span.start, '`', fence) &&
              check_after(span.end, '`', fence))
          {
            mark_before(span.start, '`', fence);
            mark_after(span.end, '`', fence);
          }
        }
        break;
      }
      default:
        break;
      }
    }

    // Detect simple <u>...</u> ranges for underline styling and remove tags.
    std::size_t searchPos = 0;
    while (searchPos < text.size())
    {
      std::size_t startLower = text.find("<u>", searchPos);
      std::size_t startUpper = text.find("<U>", searchPos);
      std::size_t start = std::min(startLower, startUpper);
      if (startLower == std::string::npos)
        start = startUpper;
      if (startUpper == std::string::npos)
        start = startLower;
      if (start == std::string::npos)
        break;
      std::size_t endLower = text.find("</u>", start + 3);
      std::size_t endUpper = text.find("</U>", start + 3);
      std::size_t end = std::min(endLower, endUpper);
      if (endLower == std::string::npos)
        end = endUpper;
      if (endUpper == std::string::npos)
        end = endLower;
      if (end == std::string::npos)
        break;

      for (std::size_t i = start; i < start + 3 && i < text.size(); ++i)
        remove[i] = true;
      for (std::size_t i = end; i < end + 4 && i < text.size(); ++i)
        remove[i] = true;
      underlineRanges.emplace_back(start + 3, end);
      searchPos = end + 4;
    }

    std::vector<int> indexMap(text.size(), -1);
    result.text.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
      if (!remove[i])
      {
        indexMap[i] = static_cast<int>(result.text.size());
        result.text.push_back(text[i]);
      }
    }

    if (result.text.empty())
    {
      result.spans.clear();
      return result;
    }

    result.spans.reserve(spans.size());
    for (const auto &span : spans)
    {
      if (span.start >= span.end || span.end > text.size())
        continue;
      std::size_t startIdx = span.start;
      std::size_t endIdx = span.end;
      int mappedStart = -1;
      for (std::size_t i = startIdx; i < endIdx; ++i)
      {
        if (i < indexMap.size() && indexMap[i] >= 0)
        {
          mappedStart = indexMap[i];
          break;
        }
      }
      if (mappedStart < 0)
        continue;
      int mappedEnd = -1;
      for (std::size_t i = endIdx; i-- > startIdx;)
      {
        if (i < indexMap.size() && indexMap[i] >= 0)
        {
          mappedEnd = indexMap[i] + 1;
          break;
        }
      }
      if (mappedEnd <= mappedStart)
        continue;
      ck::edit::MarkdownSpan adjusted = span;
      adjusted.start = static_cast<std::size_t>(mappedStart);
      adjusted.end = static_cast<std::size_t>(mappedEnd);
      result.spans.push_back(std::move(adjusted));
    }

    if (!underlineRanges.empty())
    {
      for (const auto &range : underlineRanges)
      {
        std::size_t startIdx = range.first;
        std::size_t endIdx = range.second;
        int mappedStart = -1;
        for (std::size_t i = startIdx; i < endIdx; ++i)
        {
          if (i < indexMap.size() && indexMap[i] >= 0)
          {
            mappedStart = indexMap[i];
            break;
          }
        }
        if (mappedStart < 0)
          continue;
        int mappedEnd = -1;
        for (std::size_t i = endIdx; i-- > startIdx;)
        {
          if (i < indexMap.size() && indexMap[i] >= 0)
          {
            mappedEnd = indexMap[i] + 1;
            break;
          }
        }
        if (mappedEnd <= mappedStart)
          continue;
        ck::edit::MarkdownSpan underlineSpan;
        underlineSpan.kind = ck::edit::MarkdownSpanKind::InlineHtml;
        underlineSpan.start = static_cast<std::size_t>(mappedStart);
        underlineSpan.end = static_cast<std::size_t>(mappedEnd);
        underlineSpan.attribute = "underline";
        result.spans.push_back(std::move(underlineSpan));
      }
    }

    return result;
  }

  std::vector<LinkRange> collapse_markdown_links(InlineContent &content)
  {
    std::vector<LinkRange> links;
    if (content.text.empty())
      return links;

    std::vector<bool> remove(content.text.size(), false);
    struct LinkCandidate
    {
      std::size_t labelStart = 0;
      std::size_t labelEnd = 0;
      std::string url;
    };
    std::vector<LinkCandidate> candidates;

    for (const auto &span : content.spans)
    {
      if (span.kind != ck::edit::MarkdownSpanKind::Link)
        continue;
      std::size_t spanStart = span.start;
      std::size_t spanEnd = std::min(span.end, content.text.size());
      if (spanStart >= spanEnd)
        continue;

      std::size_t pos = spanStart;
      if (content.text[pos] == '[')
      {
        remove[pos] = true;
        ++pos;
      }
      std::size_t labelStart = pos;
      int depth = 1;
      while (pos < spanEnd)
      {
        char ch = content.text[pos];
        if (ch == '[')
          ++depth;
        else if (ch == ']')
        {
          --depth;
          if (depth == 0)
            break;
        }
        ++pos;
      }
      std::size_t labelEnd = pos;
      if (pos < spanEnd && content.text[pos] == ']')
      {
        remove[pos] = true;
        ++pos;
      }
      while (pos < spanEnd &&
             std::isspace(static_cast<unsigned char>(content.text[pos])))
      {
        remove[pos] = true;
        ++pos;
      }
      if (pos < spanEnd && content.text[pos] == '(')
      {
        remove[pos] = true;
        ++pos;
        int parenDepth = 1;
        while (pos < spanEnd && parenDepth > 0)
        {
          char ch = content.text[pos];
          if (ch == '(')
          {
            ++parenDepth;
            remove[pos] = true;
            ++pos;
          }
          else if (ch == ')')
          {
            --parenDepth;
            if (parenDepth == 0)
            {
              remove[pos] = true;
              ++pos;
              break;
            }
            else
            {
              remove[pos] = true;
              ++pos;
            }
          }
          else
          {
            remove[pos] = true;
            ++pos;
          }
        }
      }
      else
      {
        // Autolink style [url] without parentheses: only brackets removed.
      }

      candidates.push_back({labelStart, labelEnd, span.attribute});
    }

    if (candidates.empty())
      return links;

    std::vector<int> indexMap(content.text.size(), -1);
    std::string collapsed;
    collapsed.reserve(content.text.size());
    for (std::size_t i = 0; i < content.text.size(); ++i)
    {
      if (!remove[i])
      {
        indexMap[i] = static_cast<int>(collapsed.size());
        collapsed.push_back(content.text[i]);
      }
    }

    std::vector<ck::edit::MarkdownSpan> adjustedSpans;
    adjustedSpans.reserve(content.spans.size());
    for (const auto &span : content.spans)
    {
      std::size_t spanStart = span.start;
      std::size_t spanEnd = std::min(span.end, content.text.size());
      int mappedStart = -1;
      for (std::size_t i = spanStart; i < spanEnd; ++i)
      {
        if (i < indexMap.size() && indexMap[i] >= 0)
        {
          mappedStart = indexMap[i];
          break;
        }
      }
      if (mappedStart < 0)
        continue;
      int mappedEnd = -1;
      for (std::size_t i = spanEnd; i-- > spanStart;)
      {
        if (i < indexMap.size() && indexMap[i] >= 0)
        {
          mappedEnd = indexMap[i] + 1;
          break;
        }
      }
      if (mappedEnd <= mappedStart)
        continue;
      ck::edit::MarkdownSpan adjusted = span;
      adjusted.start = static_cast<std::size_t>(mappedStart);
      adjusted.end = static_cast<std::size_t>(mappedEnd);
      adjustedSpans.push_back(std::move(adjusted));
    }

    for (const auto &candidate : candidates)
    {
      int mappedStart = -1;
      int mappedEnd = -1;
      for (std::size_t i = candidate.labelStart; i < candidate.labelEnd; ++i)
      {
        if (i < indexMap.size() && indexMap[i] >= 0)
        {
          if (mappedStart < 0)
            mappedStart = indexMap[i];
          mappedEnd = indexMap[i] + 1;
        }
      }
      if (mappedStart < 0 || mappedEnd <= mappedStart)
        continue;
      links.push_back(
          LinkRange{static_cast<std::size_t>(mappedStart),
                    static_cast<std::size_t>(mappedEnd), candidate.url});
    }

    content.text = std::move(collapsed);
    content.spans = std::move(adjustedSpans);
    content.links = links;
    return links;
  }

  std::string shell_escape(const std::string &value)
  {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('\'');
    for (char ch : value)
    {
      if (ch == '\'')
        escaped.append("'\\''");
      else
        escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
  }

  void open_hyperlink(const std::string &url)
  {
#ifdef _WIN32
    std::string sanitized = url;
    for (char &ch : sanitized)
    {
      if (ch == '"')
        ch = ' ';
    }
    std::string command = "start \"\" \"" + sanitized + "\"";
    std::system(command.c_str());
#elif defined(__APPLE__)
    std::string command = "open " + shell_escape(url) + " >/dev/null 2>&1 &";
    std::system(command.c_str());
#else
    std::string command = "xdg-open " + shell_escape(url) +
                          " >/dev/null 2>&1 &";
    std::system(command.c_str());
#endif
  }

  int compute_markdown_display_width(const std::string &text)
  {
    if (text.empty())
      return 0;
    ck::edit::MarkdownAnalyzer analyzer;
    ck::edit::MarkdownParserState state{};
    int maxWidth = 0;
    std::size_t offset = 0;
    while (offset <= text.size())
    {
      std::size_t newline = text.find('\n', offset);
      std::string line;
      if (newline == std::string::npos)
      {
        line = text.substr(offset);
        offset = text.size() + 1;
      }
      else
      {
        line = text.substr(offset, newline - offset);
        offset = newline + 1;
      }
      auto info = analyzer.analyzeLine(line, state);
      InlineContent processed =
          sanitize_inline_markup(info.inlineText, info.spans);
      int width = display_width(processed.text);
      if (width > maxWidth)
        maxWidth = width;
    }
    return maxWidth;
  }

  bool is_final_channel(std::string_view channel)
  {
    std::string trimmed = trim_copy(channel);
    std::string lowered = to_lower_copy(trimmed);
    return lowered.empty() || lowered == "final";
  }

  std::string sanitize_for_display(const std::string &input)
  {
    std::string output;
    output.reserve(input.size());

    const unsigned char *data =
        reinterpret_cast<const unsigned char *>(input.data());
    std::size_t len = input.size();
    std::size_t i = 0;

    auto append_ascii = [&](char ch)
    {
      if (ch == '\r')
        return;
      if (ch >= 0x20 || ch == '\n' || ch == '\t')
        output.push_back(ch);
      else
        output.push_back(' ');
    };

    while (i < len)
    {
      unsigned char byte = data[i];
      if (byte < 0x80)
      {
        append_ascii(static_cast<char>(byte));
        ++i;
        continue;
      }

      std::size_t startIndex = i;
      std::size_t extra = 0;
      uint32_t codepoint = 0;
      if ((byte & 0xE0) == 0xC0)
      {
        extra = 1;
        codepoint = byte & 0x1F;
      }
      else if ((byte & 0xF0) == 0xE0)
      {
        extra = 2;
        codepoint = byte & 0x0F;
      }
      else if ((byte & 0xF8) == 0xF0)
      {
        extra = 3;
        codepoint = byte & 0x07;
      }
      else
      {
        output.push_back('?');
        ++i;
        continue;
      }

      if (i + extra >= len)
      {
        output.push_back('?');
        break;
      }

      bool valid = true;
      for (std::size_t j = 1; j <= extra; ++j)
      {
        unsigned char follow = data[i + j];
        if ((follow & 0xC0) != 0x80)
        {
          valid = false;
          break;
        }
        codepoint = (codepoint << 6) | (follow & 0x3F);
      }

      if (!valid)
      {
        output.push_back('?');
        ++i;
        continue;
      }

      i += extra + 1;

      auto append_replacement = [&](char ch)
      {
        if (ch == '\r')
          return;
        output.push_back(ch);
      };

        output.append(input, startIndex, extra + 1);
    }

    return output;
  }

  std::string last_non_empty_line(const std::string &text)
  {
    std::string sanitized = sanitize_for_display(text);
    std::string_view view(sanitized);
    if (view.empty())
      return std::string();

    std::size_t pos = view.size();
    while (pos > 0)
    {
      std::size_t newlinePos = view.rfind('\n', pos - 1);
      std::size_t start =
          (newlinePos == std::string_view::npos) ? 0 : newlinePos + 1;
      std::string_view candidate = view.substr(start, pos - start);
      candidate = trim_view(candidate);
      if (!candidate.empty())
        return std::string(candidate);
      if (newlinePos == std::string_view::npos)
        break;
      pos = newlinePos;
    }
    return std::string(trim_view(view));
  }

  std::vector<ChannelSegment> parse_harmony_segments(const std::string &content)
  {
    std::vector<ChannelSegment> segments;
    const std::string startToken = "<|start|>assistant<|channel|>";
    const std::string messageToken = "<|message|>";
    const std::string endToken = "<|end|>";

    std::size_t pos = 0;
    while (pos < content.size())
    {
      std::size_t start = content.find("<|start|>", pos);
      if (start == std::string::npos)
      {
        std::string trailing = content.substr(pos);
        if (!trailing.empty())
          segments.push_back(ChannelSegment{"", std::move(trailing), false});
        break;
      }

      if (start > pos)
      {
        std::string plain = content.substr(pos, start - pos);
        if (!plain.empty())
          segments.push_back(ChannelSegment{"", std::move(plain), false});
      }

      if (content.compare(start, startToken.size(), startToken) != 0)
      {
        // Not a harmony assistant marker; treat as literal character.
        std::string literal = content.substr(start, 1);
        segments.push_back(ChannelSegment{"", std::move(literal)});
        pos = start + 1;
        continue;
      }

      std::size_t channelStart = start + startToken.size();
      std::size_t messagePos = content.find(messageToken, channelStart);
      if (messagePos == std::string::npos)
      {
        std::string remainder = content.substr(start);
        if (!remainder.empty())
          segments.push_back(ChannelSegment{"", std::move(remainder), false});
        break;
      }

      std::string channel =
          content.substr(channelStart, messagePos - channelStart);
      std::size_t bodyStart = messagePos + messageToken.size();
      std::size_t endPos = content.find(endToken, bodyStart);
      if (endPos == std::string::npos)
      {
        std::string body = content.substr(bodyStart);
        segments.push_back(ChannelSegment{std::move(channel), std::move(body)});
        break;
      }

      std::string body = content.substr(bodyStart, endPos - bodyStart);
      segments.push_back(
          ChannelSegment{std::move(channel), std::move(body), true});
      pos = endPos + endToken.size();
    }

    return segments;
  }

  void applyStyleRange(std::vector<StyleMask> &styles, std::size_t start,
                       std::size_t end, StyleMask mask)
  {
    if (start >= end)
      return;
    if (end > styles.size())
      end = styles.size();
    for (std::size_t i = start; i < end; ++i)
      styles[i] |= mask;
  }

  std::vector<StyledLine> wrapStyledLine(const std::string &text,
                                         const std::vector<StyleMask> &styles,
                                         const std::vector<LinkRange> &links,
                                         int width, bool hardWrap)
  {
    struct Glyph
    {
      std::size_t start = 0;
      std::size_t end = 0;
      uint32_t codepoint = 0;
      int displayWidth = 0;
    };

    std::vector<Glyph> glyphs;
    glyphs.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size())
    {
      std::size_t start = index;
      uint32_t cp = next_codepoint(text, index);
      if (cp == '\r')
        continue;
      Glyph glyph;
      glyph.start = start;
      glyph.end = index;
      glyph.codepoint = cp;
      glyph.displayWidth = codepoint_display_width(cp);
      glyphs.push_back(glyph);
    }

    std::vector<StyledLine> result;
    if (width <= 0)
      width = 1;

    std::size_t glyphIndex = 0;
    while (glyphIndex < glyphs.size())
    {
      if (glyphs[glyphIndex].codepoint == '\n')
      {
        if (result.empty() || !result.back().text.empty())
          result.push_back(StyledLine{});
        ++glyphIndex;
        continue;
      }

      std::size_t lineStart = glyphIndex;
      std::size_t wrapGlyph = lineStart;
      int currentWidth = 0;
      std::size_t lastBreakGlyph = std::numeric_limits<std::size_t>::max();
      int widthAtLastBreak = 0;

      while (wrapGlyph < glyphs.size())
      {
        const Glyph &glyph = glyphs[wrapGlyph];
        if (glyph.codepoint == '\n')
          break;

        bool isBreakableSpace =
            (glyph.codepoint == ' ' || glyph.codepoint == '\t');
        if (!hardWrap && isBreakableSpace)
        {
          lastBreakGlyph = wrapGlyph + 1;
          widthAtLastBreak = currentWidth + glyph.displayWidth;
        }

        if (width > 0 && currentWidth + glyph.displayWidth > width)
        {
          if (!hardWrap && lastBreakGlyph != std::numeric_limits<std::size_t>::max() &&
              widthAtLastBreak <= width)
          {
            wrapGlyph = lastBreakGlyph;
          }
          else if (currentWidth == 0)
          {
            wrapGlyph = wrapGlyph + 1;
          }
          break;
        }

        currentWidth += glyph.displayWidth;
        ++wrapGlyph;
      }

      if (wrapGlyph == lineStart)
        wrapGlyph = std::min(lineStart + 1, glyphs.size());

      std::size_t startByte = glyphs[lineStart].start;
      std::size_t endByte =
          (wrapGlyph < glyphs.size()) ? glyphs[wrapGlyph].start : text.size();

      StyledLine line;
      line.text.assign(text.begin() + startByte, text.begin() + endByte);
      line.styles.reserve(endByte - startByte);
      for (std::size_t i = startByte; i < endByte; ++i)
      {
        if (i < styles.size())
          line.styles.push_back(styles[i]);
        else
          line.styles.push_back(0);
      }
      for (const auto &link : links)
      {
        if (link.end <= startByte || link.start >= endByte)
          continue;
        LinkRange portion;
        portion.start = (link.start > startByte) ? (link.start - startByte) : 0;
        portion.end = std::min(link.end, endByte) - startByte;
        if (portion.start < portion.end)
        {
          portion.url = link.url;
          line.links.push_back(std::move(portion));
        }
      }
      result.push_back(std::move(line));

      glyphIndex = wrapGlyph;
      if (glyphIndex < glyphs.size() &&
          glyphs[glyphIndex].codepoint == '\n')
        ++glyphIndex;

      if (!hardWrap)
      {
        while (glyphIndex < glyphs.size() &&
               glyphs[glyphIndex].codepoint != '\n' &&
               (glyphs[glyphIndex].codepoint == ' ' ||
                glyphs[glyphIndex].codepoint == '\t'))
          ++glyphIndex;
      }
    }

    if (result.empty())
      result.push_back(StyledLine{});

    return result;
  }

  class MarkdownSegmentRenderer
  {
  public:
    explicit MarkdownSegmentRenderer(int wrapWidth, bool parseLinks)
        : width_(std::max(1, wrapWidth)), parseLinks_(parseLinks) {}

    std::vector<StyledLine> render(const std::string &text)
    {
      lines_.clear();
      tableBuffer_.clear();
      state_ = ck::edit::MarkdownParserState{};

      // Process text with reasonable safety limits
      if (text.size() > 100000)
      {
        // For extremely large text, just return plain text to avoid stalls
        StyledLine line;
        line.text = text.substr(0, 100000) + "...";
        line.styles.assign(line.text.size(), 0);
        lines_.push_back(std::move(line));
        return lines_;
      }

      std::size_t offset = 0;
      int lineCount = 0;
      const int maxLines = 5000; // Reasonable limit to prevent infinite loops

      while (offset < text.size() && lineCount < maxLines)
      {
        std::size_t newline = text.find('\n', offset);
        std::string line;
        if (newline == std::string::npos)
        {
          line = text.substr(offset);
          offset = text.size();
        }
        else
        {
          line = text.substr(offset, newline - offset);
          offset = newline + 1;
        }
        if (!line.empty() && line.back() == '\r')
          line.pop_back();

        processLine(line);
        lineCount++;
      }

      flushTable();
      return lines_;
    }

  private:
    struct TableRow
    {
      std::string raw;
      ck::edit::MarkdownLineInfo info;
    };

    int width_;
    bool parseLinks_ = false;
    ck::edit::MarkdownAnalyzer analyzer_;
    ck::edit::MarkdownParserState state_{};
    std::vector<StyledLine> lines_;
    std::vector<TableRow> tableBuffer_;

    void processLine(const std::string &line)
    {
      auto info = analyzer_.analyzeLine(line, state_);

      if (info.kind == ck::edit::MarkdownLineKind::TableRow ||
          info.kind == ck::edit::MarkdownLineKind::TableSeparator)
      {
        // Skip complex table rendering to prevent stalls
        if (tableBuffer_.size() < 200)
        { // Reasonable table size limit
          tableBuffer_.push_back(TableRow{line, info});
        }
        else
        {
          // Render as plain text if table is too large
          StyledLine plainLine;
          plainLine.text = line;
          plainLine.styles.assign(line.size(), 0);
          lines_.push_back(std::move(plainLine));
        }
        return;
      }

      flushTable();

      switch (info.kind)
      {
      case ck::edit::MarkdownLineKind::Blank:
        addBlankLine();
        break;
      case ck::edit::MarkdownLineKind::Heading:
        renderHeading(line, info);
        break;
      case ck::edit::MarkdownLineKind::BlockQuote:
        renderBlockQuote(line, info);
        break;
      case ck::edit::MarkdownLineKind::BulletListItem:
      case ck::edit::MarkdownLineKind::OrderedListItem:
        renderListItem(line, info);
        break;
      case ck::edit::MarkdownLineKind::CodeFenceStart:
      case ck::edit::MarkdownLineKind::CodeFenceEnd:
      case ck::edit::MarkdownLineKind::FencedCode:
      case ck::edit::MarkdownLineKind::IndentedCode:
      {
        // Check if this line is a horizontal rule within a code block
        std::string trimmed = line;
        // Remove leading/trailing whitespace
        while (!trimmed.empty() && std::isspace(trimmed.front()))
        {
          trimmed.erase(0, 1);
        }
        while (!trimmed.empty() && std::isspace(trimmed.back()))
        {
          trimmed.pop_back();
        }

        // Check if it's a horizontal rule pattern
        if (trimmed == "---" || trimmed == "***" || trimmed == "___" ||
            trimmed == "- - -" || trimmed == "* * *" || trimmed == "_ _ _")
        {
          // Render as horizontal rule even within code blocks
          renderHorizontalRule();
        }
        else
        {
          // Render code blocks as plain text to avoid performance issues
          StyledLine codeLine;
          codeLine.text = line;
          codeLine.styles.assign(line.size(), kStyleNone);
          lines_.push_back(std::move(codeLine));
        }
        break;
      }
      case ck::edit::MarkdownLineKind::HorizontalRule:
        renderHorizontalRule();
        break;
      case ck::edit::MarkdownLineKind::Paragraph:
      case ck::edit::MarkdownLineKind::Html:
      case ck::edit::MarkdownLineKind::Unknown:
      default:
        renderParagraph(line, info);
        break;
      }
    }

    void addBlankLine()
    {
      // Only add a blank line if we don't already have one at the end
      if (lines_.empty() || !lines_.back().text.empty())
      {
        StyledLine blank;
        lines_.push_back(std::move(blank));
      }
    }

    void renderHeading(const std::string &line,
                       const ck::edit::MarkdownLineInfo &info)
    {
      InlineContent processed =
          sanitize_inline_markup(info.inlineText, info.spans);
      if (parseLinks_)
        collapse_markdown_links(processed);
      std::string content = std::move(processed.text);
      StyleMask headingStyle = kStyleHeading;
      switch (info.headingLevel) {
        case 1: headingStyle |= (1u << 14); break;
        case 2: headingStyle |= (2u << 14); break;
        case 3: headingStyle |= (3u << 14); break;
        case 4: headingStyle |= (4u << 14); break;
        case 5: headingStyle |= (5u << 14); break;
        case 6: headingStyle |= (6u << 14); break;
        default: break;
      }
      std::vector<StyleMask> styles(content.size(), headingStyle);
      applyInlineSpans(styles, 0, processed.spans);
      appendWrapped(content, styles, processed.links, false);
      
      // Add a blank line after a heading unless we're in a table
      if (tableBuffer_.empty()) {
        addBlankLine();
      }
    }

    void renderParagraph(const std::string &line,
                         const ck::edit::MarkdownLineInfo &info)
    {
      InlineContent processed =
          sanitize_inline_markup(info.inlineText, info.spans);
      if (parseLinks_)
        collapse_markdown_links(processed);
      std::string content = std::move(processed.text);
      std::vector<StyleMask> styles(content.size(), kStyleNone);
      applyInlineSpans(styles, 0, processed.spans);
      appendWrapped(content, styles, processed.links, false);
    }

    void renderBlockQuote(const std::string &line,
                          const ck::edit::MarkdownLineInfo &info)
    {
      InlineContent processed =
          sanitize_inline_markup(info.inlineText, info.spans);
      if (parseLinks_)
        collapse_markdown_links(processed);
      std::string content = std::move(processed.text);

      std::string text = "> " + content;
      std::vector<StyleMask> styles(text.size(), kStyleQuote);
      applyStyleRange(styles, 0, 2, kStyleQuote | kStyleListMarker);
      if (!content.empty())
        applyInlineSpans(styles, 2, processed.spans);
      std::vector<LinkRange> links;
      if (!processed.links.empty())
      {
        links.reserve(processed.links.size());
        for (const auto &link : processed.links)
          links.push_back(LinkRange{link.start + 2, link.end + 2, link.url});
      }
      appendWrapped(text, styles, links, false);
    }

    void renderListItem(const std::string &line,
                        const ck::edit::MarkdownLineInfo &info)
    {
      std::string marker =
          info.kind == ck::edit::MarkdownLineKind::OrderedListItem
              ? info.marker
              : std::string();
      InlineContent processed =
          sanitize_inline_markup(info.inlineText, info.spans);
      if (parseLinks_)
        collapse_markdown_links(processed);
      std::string content = std::move(processed.text);

      bool taskCompleted = false;
      if (info.isTask)
      {
        std::size_t bracket = content.find('[');
        if (bracket != std::string::npos && bracket + 1 < content.size())
        {
          char status = content[bracket + 1];
          taskCompleted = (status == 'x' || status == 'X');
        }
      }

      if (marker.empty())
        marker = info.isTask ? (taskCompleted ? "☑" : "☐") : "•";

      std::string text = marker + " " + content;
      std::vector<StyleMask> styles(text.size(), kStyleNone);
      applyStyleRange(styles, 0, marker.size(),
                      kStyleListMarker | (info.isTask ? kStylePrefix : 0));
      applyStyleRange(styles, marker.size(), marker.size() + 1, kStyleListMarker);
      applyInlineSpans(styles, marker.size() + 1, processed.spans);
      std::vector<LinkRange> links;
      if (!processed.links.empty())
      {
        links.reserve(processed.links.size());
        std::size_t offset = marker.size() + 1;
        for (const auto &link : processed.links)
          links.push_back(
              LinkRange{link.start + offset, link.end + offset, link.url});
      }
      appendWrapped(text, styles, links, false);
    }

    void renderCode(const std::string &line,
                    const ck::edit::MarkdownLineInfo &info)
    {
      std::string text = line;
      if (info.kind == ck::edit::MarkdownLineKind::CodeFenceStart)
      {
        text = "```";
        if (!info.language.empty())
        {
          text.append(" ");
          text.append(info.language);
        }
      }
      else if (info.kind == ck::edit::MarkdownLineKind::CodeFenceEnd)
      {
        text = "```";
      }

      std::vector<StyleMask> styles(text.size(), kStyleCodeBlock);
      appendWrapped(text, styles, std::vector<LinkRange>{}, true);
    }

    void renderHorizontalRule()
    {
      std::string text;
      text.reserve(width_);
      for (int i = 0; i < width_; ++i)
        text.append(kBoxHorizontal);
      std::vector<StyleMask> styles(text.size(), kStyleHorizontalRule);
      appendWrapped(text, styles, std::vector<LinkRange>{}, true);
    }

    void applyInlineSpans(std::vector<StyleMask> &styles, std::size_t offset,
                          const std::vector<ck::edit::MarkdownSpan> &spans)
    {
      for (const auto &span : spans)
      {
        StyleMask mask = kStyleNone;
        switch (span.kind)
        {
        case ck::edit::MarkdownSpanKind::Bold:
          mask |= kStyleBold;
          break;
        case ck::edit::MarkdownSpanKind::Italic:
          mask |= kStyleItalic;
          break;
        case ck::edit::MarkdownSpanKind::BoldItalic:
          mask |= kStyleBold | kStyleItalic;
          break;
        case ck::edit::MarkdownSpanKind::Strikethrough:
          mask |= kStyleStrikethrough;
          break;
        case ck::edit::MarkdownSpanKind::Code:
          mask |= kStyleInlineCode;
          break;
        case ck::edit::MarkdownSpanKind::Link:
          mask |= kStyleLink;
          break;
        case ck::edit::MarkdownSpanKind::InlineHtml:
          if (span.attribute == "underline")
            mask |= kStyleUnderline;
          else
            mask |= kStyleLink;
          break;
        default:
          break;
        }
        if (mask != kStyleNone)
          applyStyleRange(styles, offset + span.start, offset + span.end, mask);
      }
    }

    void appendWrapped(const std::string &text,
                       const std::vector<StyleMask> &styles,
                       const std::vector<LinkRange> &links, bool hardWrap)
    {
      auto wrapped = wrapStyledLine(text, styles, links, width_, hardWrap);
      lines_.insert(lines_.end(), wrapped.begin(), wrapped.end());
    }

    void flushTable()
    {
      if (tableBuffer_.empty())
        return;

      // Safety check: limit table size to prevent performance issues
      if (tableBuffer_.size() > 100)
      {
        // For very large tables, just render as plain text to avoid stalls
        for (const auto &row : tableBuffer_)
        {
          StyledLine line;
          line.text = row.raw;
          line.styles.assign(line.text.size(), 0);
          lines_.push_back(std::move(line));
        }
        tableBuffer_.clear();
        return;
      }

      // Remove any leading rows that are not actual table data (e.g., markdown
      // prose that was misclassified while streaming).
      while (!tableBuffer_.empty())
      {
        const auto &front = tableBuffer_.front();
        if (front.info.kind == ck::edit::MarkdownLineKind::TableRow ||
            front.info.kind == ck::edit::MarkdownLineKind::TableSeparator)
          break;
        tableBuffer_.erase(tableBuffer_.begin());
      }

      std::size_t rawColumnCount = 0;
      for (const auto &row : tableBuffer_)
      {
        rawColumnCount = std::max(rawColumnCount, row.info.tableCells.size());
      }
      if (rawColumnCount == 0)
      {
        tableBuffer_.clear();
        return;
      }

      std::vector<bool> columnHasContent(rawColumnCount, false);
      for (const auto &row : tableBuffer_)
      {
        for (std::size_t i = 0; i < row.info.tableCells.size(); ++i)
        {
          const auto &cell = row.info.tableCells[i];
          std::string normalized = normalize_html_line_breaks(cell.text);
          if (!trim_copy(normalized).empty())
            columnHasContent[i] = true;
        }
      }

      std::vector<std::size_t> activeColumns;
      activeColumns.reserve(rawColumnCount);
      for (std::size_t i = 0; i < rawColumnCount; ++i)
      {
        if (columnHasContent[i])
          activeColumns.push_back(i);
      }
      if (activeColumns.empty())
      {
        for (std::size_t i = 0; i < rawColumnCount; ++i)
          activeColumns.push_back(i);
      }

      std::size_t columnCount = activeColumns.size();
      if (columnCount == 0)
      {
        tableBuffer_.clear();
        return;
      }

      // Safety check: limit column count to prevent excessive processing
      if (columnCount > 20)
      {
        // For tables with too many columns, render as plain text
        for (const auto &row : tableBuffer_)
        {
          StyledLine line;
          line.text = row.raw;
          line.styles.assign(line.text.size(), 0);
          lines_.push_back(std::move(line));
        }
        tableBuffer_.clear();
        return;
      }

      std::vector<int> colWidths(columnCount, 1);
      for (const auto &row : tableBuffer_)
      {
        for (std::size_t idx = 0; idx < columnCount; ++idx)
        {
          std::size_t sourceIndex = activeColumns[idx];
          if (sourceIndex >= row.info.tableCells.size())
            continue;
          const auto &cell = row.info.tableCells[sourceIndex];
          std::string normalized = normalize_html_line_breaks(cell.text);
          int measuredWidth = compute_markdown_display_width(normalized);
          colWidths[idx] = std::max(colWidths[idx], measuredWidth);
        }
      }

      if (table_debug_enabled())
      {
        auto &log = debug_log_stream();
        std::ostream *out = log ? static_cast<std::ostream *>(&log)
                                : static_cast<std::ostream *>(&std::cerr);
        *out << "[ck-chat][table] columns=" << columnCount
             << " raw_width=" << width_ << "\n";
        for (std::size_t idx = 0; idx < columnCount; ++idx)
          *out << "  col[" << idx << "] width=" << colWidths[idx] << "\n";
        for (const auto &row : tableBuffer_)
        {
          *out << "  row kind=" << static_cast<int>(row.info.kind)
               << " cells=" << row.info.tableCells.size() << "\n";
          for (std::size_t idx = 0; idx < row.info.tableCells.size(); ++idx)
          {
            const auto &cell = row.info.tableCells[idx];
            std::string normalized = normalize_html_line_breaks(cell.text);
            *out << "    cell[" << idx << "] text='" << normalized
                 << "' display_width="
                 << compute_markdown_display_width(normalized) << "\n";
          }
        }
        if (log)
          log.flush();
      }

      int borderWidth = static_cast<int>(columnCount) + 1;
      int totalWidth = borderWidth;
      for (int w : colWidths)
        totalWidth += w + 2;

      if (totalWidth > width_)
      {
        int available = width_ - borderWidth;
        if (available < static_cast<int>(columnCount))
          available = static_cast<int>(columnCount);
        for (std::size_t i = 0; i < colWidths.size(); ++i)
        {
          int maxAllow = std::max(1, available / static_cast<int>(columnCount));
          if (colWidths[i] > maxAllow)
            colWidths[i] = maxAllow;
        }
      }

      auto makeBorder = [&](std::string_view left, std::string_view join,
                            std::string_view right,
                            std::string_view horizontal)
      {
        std::string border;
        border.reserve(static_cast<std::size_t>(totalWidth));
        std::vector<StyleMask> styles;
        styles.reserve(border.capacity());

        append_text_with_style(border, styles, left, kStyleTableBorder);
        for (std::size_t c = 0; c < columnCount; ++c)
        {
          append_repeat_text_with_style(border, styles, horizontal,
                                        kStyleTableBorder, colWidths[c] + 2);
          if (c + 1 < columnCount)
            append_text_with_style(border, styles, join, kStyleTableBorder);
          else
            append_text_with_style(border, styles, right, kStyleTableBorder);
        }
        appendWrapped(border, styles, std::vector<LinkRange>{}, true);
      };

      makeBorder(kBoxTopLeft, kBoxTopJoin, kBoxTopRight, kBoxHorizontal);

      bool headerRendered = false;
      for (const auto &row : tableBuffer_)
      {
        bool isSeparator =
            row.info.kind == ck::edit::MarkdownLineKind::TableSeparator;
        if (isSeparator)
        {
          makeBorder(kBoxHeaderLeft, kBoxHeaderJoin, kBoxHeaderRight,
                     kBoxHeaderHorizontal);
          headerRendered = true;
          continue;
        }

        bool headerStyle = row.info.isTableHeader && !headerRendered;

        std::vector<std::vector<StyledLine>> cellStyledLines(columnCount);
        for (std::size_t c = 0; c < columnCount; ++c)
        {
          std::size_t sourceIndex = activeColumns[c];
          std::string cellText;
          if (sourceIndex < row.info.tableCells.size())
            cellText =
                normalize_html_line_breaks(row.info.tableCells[sourceIndex].text);

          // Process markdown formatting within table cells
          if (cellText.length() >= 3 &&
              cellText.find_first_of("#*`[]()-") != std::string::npos)
          {
            // Process markdown within the cell
            MarkdownSegmentRenderer cellRenderer(colWidths[c], parseLinks_);
            cellStyledLines[c] = cellRenderer.render(cellText);
          }
          else
          {
            // Plain text cell
            StyledLine plainLine;
            plainLine.text = cellText;
            plainLine.styles.assign(cellText.size(), kStyleTableCell);
            cellStyledLines[c].push_back(std::move(plainLine));
          }

          // Wrap the styled lines to fit column width
          std::vector<StyledLine> wrappedLines;
          for (const auto &styledLine : cellStyledLines[c])
          {
            auto wrapped = wrapStyledLine(styledLine.text, styledLine.styles,
                                          styledLine.links, colWidths[c], false);
            for (auto &wrappedLine : wrapped)
            {
              int lineDisplayWidth = display_width(wrappedLine.text);
              if (lineDisplayWidth < colWidths[c])
              {
                int diff = colWidths[c] - lineDisplayWidth;
                std::size_t previousSize = wrappedLine.text.size();
                wrappedLine.text.append(static_cast<std::size_t>(diff), ' ');
                wrappedLine.styles.resize(wrappedLine.text.size(),
                                          kStyleTableCell);
                for (std::size_t s = previousSize; s < wrappedLine.styles.size();
                     ++s)
                  wrappedLine.styles[s] = kStyleTableCell;
              }
              wrappedLines.push_back(std::move(wrappedLine));
            }
          }
          cellStyledLines[c] = std::move(wrappedLines);

          if (cellStyledLines[c].empty())
          {
            StyledLine emptyLine;
            emptyLine.text =
                std::string(static_cast<std::size_t>(colWidths[c]), ' ');
            emptyLine.styles.assign(emptyLine.text.size(), kStyleTableCell);
            cellStyledLines[c].push_back(std::move(emptyLine));
          }
        }

        std::size_t rowHeight = 0;
        for (const auto &cl : cellStyledLines)
          rowHeight = std::max(rowHeight, cl.size());

        for (std::size_t r = 0; r < rowHeight; ++r)
        {
          std::string line;
          std::vector<StyleMask> styles;
          line.reserve(static_cast<std::size_t>(totalWidth) * 3);
          styles.reserve(static_cast<std::size_t>(totalWidth) * 3);

          append_text_with_style(line, styles, kBoxVertical, kStyleTableBorder);

          for (std::size_t c = 0; c < columnCount; ++c)
          {
            line.push_back(' ');
            styles.push_back(headerStyle ? kStyleTableHeader : kStyleTableCell);

            const auto &cl = cellStyledLines[c];
            if (r < cl.size())
            {
              // Use the styled content from the cell
              const auto &cellLine = cl[r];
              for (std::size_t i = 0; i < cellLine.text.size(); ++i)
              {
                line.push_back(cellLine.text[i]);
                // Use the cell's style, but override with table header style if
                // needed
                StyleMask cellStyle = cellLine.styles[i];
                if (headerStyle)
                {
                  cellStyle = kStyleTableHeader;
                }
                styles.push_back(cellStyle);
              }
            }
            else
            {
              // Empty cell
              std::string emptyCell(static_cast<std::size_t>(colWidths[c]), ' ');
              for (char ch : emptyCell)
              {
                line.push_back(ch);
                styles.push_back(headerStyle ? kStyleTableHeader
                                             : kStyleTableCell);
              }
            }

            line.push_back(' ');
            styles.push_back(headerStyle ? kStyleTableHeader : kStyleTableCell);

            append_text_with_style(line, styles, kBoxVertical,
                                   kStyleTableBorder);
          }

          appendWrapped(line, styles, std::vector<LinkRange>{}, true);
        }

        if (row.info.isTableHeader && !headerRendered)
          headerRendered = true;
      }

      makeBorder(kBoxBottomLeft, kBoxBottomJoin, kBoxBottomRight,
                 kBoxHorizontal);
      tableBuffer_.clear();
    }
  };

  std::vector<StyledLine> render_markdown_to_styled_lines(const std::string &text,
                                                          int wrapWidth,
                                                          bool parseLinks)
  {
    MarkdownSegmentRenderer renderer(wrapWidth, parseLinks);
    return renderer.render(text);
  }

  TColorAttr applyStyleToAttr(TColorAttr base, StyleMask mask)
  {
    TColorAttr attr = base;

    auto setFg = [&](int code)
    {
      setFore(attr, TColorDesired(TColorBIOS(code)));
    };
    auto setBg = [&](int code)
    {
      setBack(attr, TColorDesired(TColorBIOS(code)));
    };

    int fg = -1;
    int bg = -1;

    auto chooseFg = [&](int code)
    {
      if (fg == -1)
        fg = code;
    };

    if (mask & kStyleTableBorder)
      chooseFg(0x08);
    if (mask & kStyleHeading)
    {
      int headingLevel = static_cast<int>(
          (mask & kStyleHeadingLevelMask) >> kStyleHeadingLevelShift);
      if (headingLevel == 1) {
        setStyle(attr, static_cast<ushort>(getStyle(attr) | sfBold | sfUnderline));
      }
      else if (headingLevel == 2) {
        setStyle(attr, static_cast<ushort>(getStyle(attr) | sfBold));
      }
      else if (headingLevel == 3) {
        setStyle(attr, static_cast<ushort>(getStyle(attr) | sfItalic));
      }
      else {
        // h4 and following are italic
        setStyle(attr, static_cast<ushort>(getStyle(attr) | sfItalic));
      }
    }
    if (mask & kStyleInlineCode)
    {
      chooseFg(0x0A);
      bg = 0x01;
    }
    if (mask & kStyleCodeBlock)
    {
      chooseFg(0x0A);
      bg = 0x01;
    }
    if (mask & kStyleLink)
      chooseFg(0x09);
    if (mask & kStyleQuote)
      chooseFg(0x0B);
    if (mask & kStyleStrikethrough)
    {
      chooseFg(0x08);
      setStyle(
          attr,
          static_cast<ushort>(getStyle(attr) |
                                   static_cast<ushort>(slStrike)));
    }
    if (mask & kStyleItalic)
      setStyle(
          attr,
          static_cast<ushort>(getStyle(attr) |
                                   static_cast<ushort>(slItalic)));
    if (mask & kStyleUnderline)
      setStyle(
          attr,
          static_cast<ushort>(getStyle(attr) |
                                   static_cast<ushort>(slUnderline)));
    if (mask & kStyleBold)
    {
      setStyle(
          attr,
          static_cast<ushort>(getStyle(attr) |
                                   static_cast<ushort>(slBold)));
    }
    if (mask & kStylePrefix)
      fg = 0x0C;
    if (mask & kStyleHorizontalRule)
    {
      chooseFg(0x08); // Dark gray line
    }

    if (fg != -1)
      setFg(fg);
    if (bg != -1)
      setBg(bg);

    return attr;
  }

} // namespace

std::vector<ChatTranscriptView::DisplayRow::GlyphInfo>
ChatTranscriptView::buildGlyphs(const std::string &text)
{
  std::vector<DisplayRow::GlyphInfo> glyphs;
  glyphs.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size())
  {
    std::size_t start = index;
    uint32_t cp = next_codepoint(text, index);
    DisplayRow::GlyphInfo glyph;
    glyph.start = start;
    glyph.end = index;
    glyph.width = codepoint_display_width(cp);
    glyphs.push_back(glyph);
  }
  return glyphs;
}

void ChatTranscriptView::finalizeDisplayRow(DisplayRow &row)
{
  row.glyphs = buildGlyphs(row.text);
  row.displayWidth = 0;
  for (const auto &glyph : row.glyphs)
    row.displayWidth += glyph.width;
}

StyleMask ChatTranscriptView::glyphStyleMask(
    const DisplayRow &row, const DisplayRow::GlyphInfo &glyph)
{
  if (row.styleMask.empty())
    return 0;
  StyleMask mask = 0;
  std::size_t end = std::min<std::size_t>(glyph.end, row.styleMask.size());
  for (std::size_t i = glyph.start; i < end; ++i)
    mask |= row.styleMask[i];
  return mask;
}

ChatTranscriptView::ChatTranscriptView(const TRect &bounds, TScrollBar *hScroll,
                                       TScrollBar *vScroll)
    : TScroller(bounds, hScroll, vScroll)
{
  options |= ofFirstClick;
  growMode = gfGrowHiX | gfGrowHiY;
  setLimit(1, 1);
}

void ChatTranscriptView::setMessages(
    const std::vector<ck::chat::ChatSession::Message> &sessionMessages)
{
  messages.clear();
  messages.reserve(sessionMessages.size());
  for (const auto &msg : sessionMessages)
  {
    messages.push_back(Message{msg.role, msg.content, msg.pending});
  }
  layoutDirty = true;
  rebuildLayout();
}

void ChatTranscriptView::clearMessages()
{
  messages.clear();
  rows.clear();
  layoutDirty = true;
  setLimit(1, 1);
  scrollTo(0, 0);
  drawView();
  notifyLayoutChanged(false);
}

void ChatTranscriptView::scrollToBottom()
{
  rebuildLayoutIfNeeded();
  int totalRows = static_cast<int>(rows.size());
  if (totalRows <= 0)
    totalRows = 1;
  int desired = std::max(0, totalRows - size.y);
  scrollTo(delta.x, desired);
  notifyLayoutChanged(false);
}

bool ChatTranscriptView::isAtBottom() const noexcept
{
  int maxDelta = std::max(0, limit.y - size.y);
  return delta.y >= maxDelta;
}

void ChatTranscriptView::setLayoutChangedCallback(
    std::function<void(bool)> cb)
{
  layoutChangedCallback = std::move(cb);
}

void ChatTranscriptView::setHiddenDetailCallback(
    std::function<void(std::size_t, const std::string &, const std::string &)>
        cb)
{
  hiddenDetailCallback_ = std::move(cb);
}

bool ChatTranscriptView::messageForCopy(std::size_t index,
                                        std::string &out) const
{
  if (index >= messages.size())
    return false;
  const auto &msg = messages[index];
  if (msg.role != Role::Assistant)
    return false;
  auto segments = parse_harmony_segments(msg.content);
  if (!segments.empty())
  {
    std::string assembled;
    for (const auto &segment : segments)
    {
      if (is_final_channel(segment.channel))
        assembled += segment.text;
    }
    if (!assembled.empty())
    {
      out = std::move(assembled);
      return true;
    }
  }
  out = msg.content;
  return true;
}

void ChatTranscriptView::getAllMessagesForCopy(std::string &out) const
{
  std::string result;
  for (std::size_t i = 0; i < messages.size(); ++i)
  {
    const auto &msg = messages[i];
    if (msg.role == Role::User)
    {
      if (!result.empty()) result += "\n\n";
      result += "User: " + msg.content;
    }
    else if (msg.role == Role::Assistant)
    {
      std::string content;
      if (messageForCopy(i, content))
      {
        if (!result.empty()) result += "\n\n";
        result += "Assistant: " + content;
      }
    }
  }
  out = std::move(result);
}

std::optional<std::size_t>
ChatTranscriptView::lastAssistantMessageIndex() const
{
  for (std::size_t i = messages.size(); i-- > 0;)
  {
    const auto &msg = messages[i];
    if (msg.role == Role::Assistant && !msg.pending)
      return i;
  }
  return std::nullopt;
}

void ChatTranscriptView::setMessagePending(std::size_t index, bool pending)
{
  if (index >= messages.size())
    return;
  messages[index].pending = pending;
}

bool ChatTranscriptView::isMessagePending(std::size_t index) const
{
  if (index >= messages.size())
    return false;
  return messages[index].pending;
}

std::optional<int>
ChatTranscriptView::firstRowForMessage(std::size_t index) const
{
  for (std::size_t row = 0; row < rows.size(); ++row)
  {
    if (rows[row].messageIndex == index && rows[row].isFirstLine)
      return static_cast<int>(row);
  }
  return std::nullopt;
}

void ChatTranscriptView::setShowThinking(bool show)
{
  if (showThinking_ == show)
    return;
  showThinking_ = show;
  layoutDirty = true;
  rebuildLayoutIfNeeded();
  drawView();
  notifyLayoutChanged(false);
}

void ChatTranscriptView::setShowAnalysis(bool show)
{
  if (showAnalysis_ == show)
    return;
  showAnalysis_ = show;
  layoutDirty = true;
  rebuildLayoutIfNeeded();
  drawView();
  notifyLayoutChanged(false);
}

void ChatTranscriptView::setParseMarkdownLinks(bool enable)
{
  if (parseMarkdownLinks_ == enable)
    return;
  parseMarkdownLinks_ = enable;
  layoutDirty = true;
  rebuildLayoutIfNeeded();
  drawView();
  notifyLayoutChanged(false);
}

void ChatTranscriptView::draw()
{
  rebuildLayoutIfNeeded();

  auto colors = getColor(1);
  TColorAttr baseAttr = colors[0];
  // setBack(baseAttr, TColorDesired(TColorBIOS(0x01))); // blue background for transcript area

  int viewWidth = std::max(1, size.x);
  int visibleRows = size.y;
  TDrawBuffer buffer;
  for (int y = 0; y < visibleRows; ++y)
  {
    buffer.moveChar(0, ' ', baseAttr, viewWidth);
    std::size_t rowIndex = static_cast<std::size_t>(delta.y + y);
    if (rowIndex < rows.size())
    {
      const auto &row = rows[rowIndex];
      TColorAttr attr = baseAttr;
      if (row.role == Role::Assistant)
      {
        if (row.isThinking)
          setFore(attr, TColorDesired(TColorBIOS(0x08)));
        else
          setFore(attr, TColorDesired(TColorBIOS(0x01)));
      }
      if (!row.glyphs.empty())
      {
        int columnPos = 0;
        std::size_t glyphIndex = 0;
        while (glyphIndex < row.glyphs.size() && columnPos < viewWidth)
        {
          const auto &startGlyph = row.glyphs[glyphIndex];
          StyleMask mask = glyphStyleMask(row, startGlyph);
          std::size_t runStartGlyph = glyphIndex;
          std::size_t runEndGlyph = glyphIndex;
          int runColumns = 0;

          while (runEndGlyph < row.glyphs.size())
          {
            const auto &glyph = row.glyphs[runEndGlyph];
            StyleMask glyphMask = glyphStyleMask(row, glyph);
            if (runEndGlyph > runStartGlyph && glyphMask != mask)
              break;
            if (columnPos + runColumns + glyph.width > viewWidth)
              break;
            runColumns += glyph.width;
            ++runEndGlyph;
          }

          if (runColumns <= 0)
            break;

          std::size_t runStartByte = row.glyphs[runStartGlyph].start;
          std::size_t runEndByte =
              row.glyphs[runEndGlyph - 1].end;
          std::string fragment =
              row.text.substr(runStartByte, runEndByte - runStartByte);
          TColorAttr runAttr =
              (mask == 0) ? attr : applyStyleToAttr(attr, mask);
          buffer.moveStr(columnPos, TStringView(fragment), runAttr,
                         static_cast<ushort>(runColumns));

          columnPos += runColumns;
          glyphIndex = runEndGlyph;
        }
      }
    }
    writeLine(0, y, viewWidth, 1, buffer);
  }

  if (vScrollBar)
    vScrollBar->drawView();
}

void ChatTranscriptView::changeBounds(const TRect &bounds)
{
  TScroller::changeBounds(bounds);
  layoutDirty = true;
  rebuildLayoutIfNeeded();
  notifyLayoutChanged(false);
  if (vScrollBar)
    vScrollBar->drawView();
}

void ChatTranscriptView::handleEvent(TEvent &event)
{
  if (event.what == evMouseDown && (event.mouse.buttons & mbLeftButton))
  {
    rebuildLayoutIfNeeded();
    TPoint local = makeLocal(event.mouse.where);
    if (local.x >= 0 && local.x < size.x && local.y >= 0 && local.y < size.y)
    {
      std::size_t rowIndex =
          static_cast<std::size_t>(delta.y + static_cast<int>(local.y));
      if (rowIndex < rows.size())
      {
        const auto &row = rows[rowIndex];
        if (!row.links.empty())
        {
          int column = local.x;
          int cursor = 0;
          for (const auto &glyph : row.glyphs)
          {
            if (cursor + glyph.width > column)
            {
              std::size_t bytePos = glyph.start;
              for (const auto &link : row.links)
              {
                if (bytePos >= link.start && bytePos < link.end &&
                    !link.url.empty())
                {
                  open_hyperlink(link.url);
                  clearEvent(event);
                  return;
                }
              }
              break;
            }
            cursor += glyph.width;
          }
        }
      }
    }
  }

  TPoint before = delta;
  TScroller::handleEvent(event);
  if (before.x != delta.x || before.y != delta.y)
    notifyLayoutChanged(true);
}

std::string ChatTranscriptView::prefixForRole(Role role)
{
  switch (role)
  {
  case Role::User:
    return "You: ";
  case Role::Assistant:
    return "Assistant: ";
  default:
    return std::string{};
  }
}

void ChatTranscriptView::rebuildLayoutIfNeeded()
{
  if (!layoutDirty)
    return;
  rebuildLayout();
}

void ChatTranscriptView::rebuildLayout()
{
  rows.clear();
  int width = std::max(1, size.x);
  spinnerFrame_++;

  for (std::size_t i = 0; i < messages.size(); ++i)
  {
    const auto &msg = messages[i];
    bool messageFirstRow = true;

    if (msg.role == Role::Assistant)
    {
      auto segments = parse_harmony_segments(msg.content);
      bool hasMarker = std::any_of(
          segments.begin(), segments.end(),
          [](const ChannelSegment &seg)
          { return seg.from_marker; });
      bool finalSeen = false;

      constexpr std::size_t kInvalidRowIndex =
          std::numeric_limits<std::size_t>::max();

      struct HiddenAggregate
      {
        std::string channel;
        std::string text;
        bool pending = false;
        bool thinking = true;
        std::size_t rowIndex = kInvalidRowIndex;
      };
      std::vector<HiddenAggregate> hidden;
      std::unordered_map<std::string, std::size_t> hiddenIndex;

      auto ensureHiddenAggregate = [&](const std::string &channelLabel,
                                       bool thinking) -> HiddenAggregate &
      {
        std::string key =
            channelLabel.empty() ? std::string("analysis") : channelLabel;
        auto it = hiddenIndex.find(key);
        if (it == hiddenIndex.end())
        {
          hidden.push_back(HiddenAggregate{key, std::string(), msg.pending,
                                           thinking, kInvalidRowIndex});
          it = hiddenIndex.emplace(key, hidden.size() - 1).first;
        }
        auto &agg = hidden[it->second];
        agg.channel = key;
        return agg;
      };

      auto handleVisibleSegment = [&](const std::string &channel,
                                      const std::string &text, bool thinking)
      {
        std::string label = channel.empty() ? std::string("analysis") : channel;
        std::string prefix =
            thinking ? "Assistant (" + label + "): " : "Assistant: ";
        appendVisibleSegment(Role::Assistant, prefix, text, i, messageFirstRow,
                             width, thinking, label);
      };

      auto normalizedChannel = [](const std::string &label)
      {
        std::string printable = sanitize_for_display(label);
        if (printable.empty())
          printable = "analysis";
        return printable;
      };

      std::function<bool(const std::string &, bool)> shouldHide =
          [&](const std::string &channelLower, bool thinking)
      {
        if (channelLower == "analysis")
          return !showAnalysis_;
        if (thinking)
          return !showThinking_;
        return false;
      };

      if (segments.empty())
      {
        std::string original = msg.content;
        std::string channelLabel;

        const std::string channelToken = "<|channel|>";
        const std::string messageToken = "<|message|>";

        auto channelPos = original.find(channelToken);
        if (channelPos != std::string::npos)
        {
          std::size_t headerStart = channelPos + channelToken.size();
          auto messagePos = original.find(messageToken, headerStart);
          if (messagePos != std::string::npos)
          {
            channelLabel = trim_copy(
                original.substr(headerStart, messagePos - headerStart));
            original.erase(channelPos,
                           (messagePos + messageToken.size()) - channelPos);
          }
        }

        std::size_t endTag = original.find("<|end|");
        if (endTag != std::string::npos)
          original.erase(endTag);

        std::size_t startTag = original.find("<|start|");
        if (startTag != std::string::npos)
          original.erase(startTag, std::string::npos);

        std::string cleaned = sanitize_for_display(original);

        bool thinkingSegment =
            hasMarker || !channelLabel.empty() ||
            msg.content.find("<|channel|") != std::string::npos;
        std::string printableChannel = normalizedChannel(channelLabel);
        std::string channelLower = to_lower_copy(printableChannel);

        if (shouldHide(channelLower, thinkingSegment))
        {
          auto &agg =
              ensureHiddenAggregate(printableChannel, thinkingSegment);
          if (!agg.text.empty())
            agg.text.push_back('\n');
          agg.text += cleaned;
          agg.pending = msg.pending;
          agg.thinking = thinkingSegment;
          std::string trimmedHidden = trim_copy(agg.text);
          std::string prefix = "Assistant (" + agg.channel + "): ";
          if (agg.rowIndex == kInvalidRowIndex)
          {
            agg.rowIndex = appendHiddenPlaceholder(
                prefix, agg.channel, trimmedHidden, i, agg.pending,
                agg.thinking, messageFirstRow);
          }
          else
          {
            updateHiddenPlaceholder(agg.rowIndex, prefix, agg.channel,
                                    trimmedHidden, i, agg.pending,
                                    agg.thinking);
          }
          messageFirstRow = false;
        }
        else
        {
          handleVisibleSegment(printableChannel, cleaned, thinkingSegment);
        }
      }
      else
      {
        for (const auto &segment : segments)
        {
          std::string channelLabel = trim_copy(segment.channel);
          std::string text = segment.text;

          if (channelLabel.empty())
          {
            const std::string channelToken = "<|channel|>";
            const std::string messageToken = "<|message|>";
            auto channelPos = text.find(channelToken);
            if (channelPos != std::string::npos)
            {
              std::size_t headerStart = channelPos + channelToken.size();
              auto messagePos = text.find(messageToken, headerStart);
              if (messagePos != std::string::npos)
              {
                channelLabel = trim_copy(
                    text.substr(headerStart, messagePos - headerStart));
                text.erase(channelPos,
                           (messagePos + messageToken.size()) - channelPos);
              }
            }
            auto endTag = text.find("<|end|");
            if (endTag != std::string::npos)
              text.erase(endTag);
            auto startTag = text.find("<|start|");
            if (startTag != std::string::npos)
              text.erase(startTag, std::string::npos);
          }

          bool finalSegment =
              (!channelLabel.empty() && is_final_channel(channelLabel));
          bool thinkingSegment = channelLabel.empty()
                                     ? (segment.from_marker && !finalSegment) ||
                                           (hasMarker && !finalSeen)
                                     : !finalSegment;

          std::string printableChannel = normalizedChannel(channelLabel);
          std::string channelLower = to_lower_copy(printableChannel);
          std::string segmentText = sanitize_for_display(text);

          if (shouldHide(channelLower, thinkingSegment))
          {
            auto &agg =
                ensureHiddenAggregate(printableChannel, thinkingSegment);
            if (!agg.text.empty())
              agg.text.push_back('\n');
            agg.text += segmentText;
            agg.pending = msg.pending;
            agg.thinking = thinkingSegment;
            std::string trimmedHidden = trim_copy(agg.text);
            std::string prefix = "Assistant (" + agg.channel + "): ";
            if (agg.rowIndex == kInvalidRowIndex)
            {
              agg.rowIndex = appendHiddenPlaceholder(
                  prefix, agg.channel, trimmedHidden, i, agg.pending,
                  agg.thinking, messageFirstRow);
            }
            else
            {
              updateHiddenPlaceholder(agg.rowIndex, prefix, agg.channel,
                                      trimmedHidden, i, agg.pending,
                                      agg.thinking);
            }
            messageFirstRow = false;
          }
          else
          {
            handleVisibleSegment(printableChannel, segmentText,
                                 thinkingSegment);
          }

          if ((segment.from_marker || !channelLabel.empty()) && finalSegment)
            finalSeen = true;
        }
      }

    }
    else
    {
      std::string prefix = prefixForRole(msg.role);
      std::string sanitized = sanitize_for_display(msg.content);
      appendVisibleSegment(msg.role, prefix, sanitized, i, messageFirstRow,
                           width, false, std::string());
    }

    if (i + 1 < messages.size())
    {
      DisplayRow spacer;
      spacer.role = Role::System;
      spacer.text = std::string();
      spacer.messageIndex = i;
      spacer.isFirstLine = false;
      rows.push_back(std::move(spacer));
      finalizeDisplayRow(rows.back());
    }
  }

  int total = static_cast<int>(rows.size());
  if (total <= 0)
    total = 1;
  setLimit(1, total);
  layoutDirty = false;
  notifyLayoutChanged(false);
}

void ChatTranscriptView::appendVisibleSegment(
    Role role, const std::string &prefix, const std::string &text,
    std::size_t messageIndex, bool &messageFirstRow, int width, bool thinking,
    const std::string &channelLabel)
{
  int contentWidth = width - static_cast<int>(prefix.size());
  if (contentWidth < 1)
    contentWidth = 1;

  // Temporarily enable markdown rendering for all content to test formatting
  std::vector<StyledLine> styled;
  bool hasMarkdownSyntax =
      text.find_first_of("#*`[]()-") != std::string::npos;
  bool forceMarkdownLinks =
      parseMarkdownLinks_ && text.find('[') != std::string::npos &&
      text.find(')') != std::string::npos;
  if (!forceMarkdownLinks &&
      (text.length() < 10 || text.length() > 10000 || !hasMarkdownSyntax))
  {
    // Skip markdown rendering for content that might cause stalls
    // But still preserve line breaks by splitting on newlines and apply word
    // wrapping
    std::size_t offset = 0;
    while (offset < text.size())
    {
      std::size_t newline = text.find('\n', offset);
      std::string line;
      if (newline == std::string::npos)
      {
        line = text.substr(offset);
        offset = text.size();
      }
      else
      {
        line = text.substr(offset, newline - offset);
        offset = newline + 1;
      }

      // Apply word wrapping to each line, even when markdown rendering is
      // skipped
      std::vector<StyleMask> lineStyles(line.size(), 0);
      auto wrappedLines =
          wrapStyledLine(line, lineStyles, std::vector<LinkRange>{}, contentWidth,
                         false);
      styled.insert(styled.end(), wrappedLines.begin(), wrappedLines.end());
    }
  }
  else
  {
    styled = render_markdown_to_styled_lines(text, contentWidth,
                                             parseMarkdownLinks_);
  }

  if (styled.empty())
    styled.push_back(StyledLine{});

  std::string indent(prefix.size(), ' ');

  for (std::size_t idx = 0; idx < styled.size(); ++idx)
  {
    const StyledLine &line = styled[idx];
    DisplayRow row;
    row.role = role;
    row.messageIndex = messageIndex;
    row.isFirstLine = messageFirstRow && idx == 0;
    row.isThinking = thinking;
    row.channelLabel = channelLabel;

    const std::string &prefixToUse = (idx == 0) ? prefix : indent;
    row.text = prefixToUse + line.text;
    row.styleMask.assign(row.text.size(), 0);
    if (!prefixToUse.empty())
      applyStyleRange(row.styleMask, 0, prefixToUse.size(), kStylePrefix);

    for (std::size_t i = 0; i < line.text.size(); ++i)
    {
      if (prefixToUse.size() + i < row.styleMask.size())
        row.styleMask[prefixToUse.size() + i] =
            line.styles.size() > i ? line.styles[i] : 0;
    }

    if (!line.links.empty())
    {
      row.links.reserve(line.links.size());
      for (const auto &link : line.links)
      {
        DisplayRow::LinkInfo info;
        info.start = prefixToUse.size() + link.start;
        info.end = prefixToUse.size() + link.end;
        info.url = link.url;
        row.links.push_back(std::move(info));
      }
    }
    rows.push_back(std::move(row));
    finalizeDisplayRow(rows.back());
    messageFirstRow = false;
  }
}

std::size_t ChatTranscriptView::appendHiddenPlaceholder(
    const std::string &prefix, const std::string &channelLabel,
    const std::string &content, std::size_t messageIndex, bool pending,
    bool thinking, bool isFirstRow)
{
  // Show spinner during pending state regardless of hide settings
  // Only hide when not pending and the corresponding hide option is enabled
  if (!pending) {
    if (thinking && !showThinking_)
      return rows.size();
    if (!thinking && channelLabel == "analysis" && !showAnalysis_)
      return rows.size();
  }

  DisplayRow row;
  row.role = Role::Assistant;
  row.messageIndex = messageIndex;
  row.isFirstLine = isFirstRow;
  row.isPlaceholder = true;
  rows.push_back(std::move(row));
  std::size_t index = rows.size() - 1;
  updateHiddenPlaceholder(index, prefix, channelLabel, content, messageIndex,
                          pending, thinking);
  return index;
}

void ChatTranscriptView::updateHiddenPlaceholder(
    std::size_t rowIndex, const std::string &prefix,
    const std::string &channelLabel, const std::string &content,
    std::size_t messageIndex, bool pending, bool thinking)
{
  if (rowIndex >= rows.size())
    return;
  auto &row = rows[rowIndex];
  row.role = Role::Assistant;
  row.messageIndex = messageIndex;
  row.isThinking = thinking;
  row.isPlaceholder = true;
  row.isPending = pending;
  row.channelLabel = channelLabel;
  row.hiddenContent = content.empty() ? std::string("(no content)") : content;
  row.links.clear();

  static const char* spinnerChars[] = {
      "⣷", "⣯", "⣟", "⡿", "⢿", "⣻", "⣽", "⣾"  // Reversed order
  };
  
  // Only update spinner every N rebuilds to slow down animation
  static const int SPINNER_SPEED_DIVIDER = 3;
  if (pending)
  {
    const char* frame = spinnerChars[(spinnerFrame_ / SPINNER_SPEED_DIVIDER) %
                                   (sizeof(spinnerChars) / sizeof(spinnerChars[0]))];
    row.text = prefix + "Generating… " + frame;
  }
  else
  {
    row.text = prefix + "[Analysis finished – click to view]";
  }

  row.styleMask.assign(row.text.size(), 0);
  if (!prefix.empty())
    applyStyleRange(row.styleMask, 0, prefix.size(), kStylePrefix);

  if (pending)
  {
    const char* frame = spinnerChars[(spinnerFrame_ / SPINNER_SPEED_DIVIDER) %
                                   (sizeof(spinnerChars) / sizeof(spinnerChars[0]))];
    std::size_t spinnerPos = row.text.size() - strlen(frame);
    for (std::size_t i = spinnerPos; i < row.text.size(); i++)
      row.styleMask[i] = kStyleBold;
  }
  finalizeDisplayRow(row);
}

void ChatTranscriptView::openHiddenRow(std::size_t rowIndex)
{
  if (rowIndex >= rows.size())
    return;
  const auto &row = rows[rowIndex];
  if (!row.isPlaceholder || row.isPending || !hiddenDetailCallback_)
    return;
  hiddenDetailCallback_(row.messageIndex, row.channelLabel, row.hiddenContent);
}

void ChatTranscriptView::notifyLayoutChanged(bool userScroll)
{
  if (layoutChangedCallback)
    layoutChangedCallback(userScroll);
}
