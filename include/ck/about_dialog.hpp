#pragma once

#ifndef Uses_MsgBox
#define CK_ABOUT_DIALOG_DEFINE_USES_MSGBOX
#define Uses_MsgBox
#endif
#ifndef Uses_TButton
#define CK_ABOUT_DIALOG_DEFINE_USES_TBUTTON
#define Uses_TButton
#endif
#ifndef Uses_TDialog
#define CK_ABOUT_DIALOG_DEFINE_USES_TDIALOG
#define Uses_TDialog
#endif
#ifndef Uses_TDeskTop
#define CK_ABOUT_DIALOG_DEFINE_USES_TDESKTOP
#define Uses_TDeskTop
#endif
#ifndef Uses_TDrawBuffer
#define CK_ABOUT_DIALOG_DEFINE_USES_TDRAWBUFFER
#define Uses_TDrawBuffer
#endif
#ifndef Uses_TObject
#define CK_ABOUT_DIALOG_DEFINE_USES_TOBJECT
#define Uses_TObject
#endif
#ifndef Uses_TProgram
#define CK_ABOUT_DIALOG_DEFINE_USES_TPROGRAM
#define Uses_TProgram
#endif
#ifndef Uses_TRect
#define CK_ABOUT_DIALOG_DEFINE_USES_TRECT
#define Uses_TRect
#endif
#ifndef Uses_TStaticText
#define CK_ABOUT_DIALOG_DEFINE_USES_TSTATICTEXT
#define Uses_TStaticText
#endif
#include <tvision/tv.h>
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TSTATICTEXT
#undef Uses_TStaticText
#undef CK_ABOUT_DIALOG_DEFINE_USES_TSTATICTEXT
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TRECT
#undef Uses_TRect
#undef CK_ABOUT_DIALOG_DEFINE_USES_TRECT
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TPROGRAM
#undef Uses_TProgram
#undef CK_ABOUT_DIALOG_DEFINE_USES_TPROGRAM
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TOBJECT
#undef Uses_TObject
#undef CK_ABOUT_DIALOG_DEFINE_USES_TOBJECT
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TDRAWBUFFER
#undef Uses_TDrawBuffer
#undef CK_ABOUT_DIALOG_DEFINE_USES_TDRAWBUFFER
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TDIALOG
#undef Uses_TDialog
#undef CK_ABOUT_DIALOG_DEFINE_USES_TDIALOG
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TDESKTOP
#undef Uses_TDeskTop
#undef CK_ABOUT_DIALOG_DEFINE_USES_TDESKTOP
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_TBUTTON
#undef Uses_TButton
#undef CK_ABOUT_DIALOG_DEFINE_USES_TBUTTON
#endif
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_MSGBOX
#undef Uses_MsgBox
#undef CK_ABOUT_DIALOG_DEFINE_USES_MSGBOX
#endif

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ck::ui
{
    struct AboutDialogInfo
    {
        std::string_view toolName;
        std::string_view version;
        std::string_view description;
        std::string_view copyright = "© 2025 by Dr. C. Klukas";
        std::string_view applicationName = "CK Utilities";
        std::string_view buildDate = __DATE__;
        std::string_view buildTime = __TIME__;
    };

    inline std::string buildAboutDialogMessage(const AboutDialogInfo &info)
    {
        std::vector<std::string> paragraphs;
        {
            std::ostringstream headerOut;
            headerOut << info.applicationName;
            if (!info.applicationName.empty() && !info.copyright.empty())
                headerOut << ' ';
            if (!info.copyright.empty())
                headerOut << info.copyright;
            paragraphs.emplace_back(headerOut.str());
        }
        if (!info.toolName.empty())
            paragraphs.emplace_back(info.toolName);

        if (!info.description.empty())
            paragraphs.emplace_back(std::string(info.description));

        if (!info.version.empty())
        {
            std::ostringstream versionOut;
            versionOut << "Version: " << info.version;
            paragraphs.emplace_back(versionOut.str());
        }

        if (!info.buildDate.empty())
        {
            std::ostringstream buildOut;
            buildOut << "Build: " << info.buildDate;
            if (!info.buildTime.empty())
                buildOut << ' ' << info.buildTime;
            paragraphs.emplace_back(buildOut.str());
        }

        std::ostringstream out;
        for (size_t i = 0; i < paragraphs.size(); ++i)
        {
            if (i > 0)
                out << "\n\n";
            out << paragraphs[i];
        }

        return out.str();
    }

    namespace detail
    {

        class AboutStaticText : public TStaticText
        {
        public:
            AboutStaticText(const TRect &bounds,
                            TStringView message,
                            std::vector<std::string> lines,
                            size_t highlightIndex,
                            size_t highlightPrefixLength) noexcept
                : TStaticText(bounds, message), m_lines(std::move(lines)), m_highlightIndex(highlightIndex), m_highlightPrefixLength(highlightPrefixLength)
            {
                growMode |= gfFixed;
            }

            void draw() override
            {
                const TColorAttr normal = getColor(1);
                TColorAttr highlight = normal;
                ::setFore(highlight, TColorBIOS(0x1));
                TDrawBuffer buffer;
                for (int y = 0; y < size.y; ++y)
                {
                    buffer.moveChar(0, ' ', normal, size.x);
                    if (y < static_cast<int>(m_lines.size()))
                    {
                        const bool isHighlighted = (m_highlightIndex < m_lines.size()) &&
                                                   (static_cast<size_t>(y) == m_highlightIndex) &&
                                                   !m_lines[y].empty();
                        if (isHighlighted && m_highlightPrefixLength > 0)
                        {
                            const std::string &line = m_lines[y];
                            const size_t prefixLength = std::min(m_highlightPrefixLength, line.size());
                            const TStringView prefix(line.data(), prefixLength);
                            buffer.moveStr(0, prefix, highlight);

                            const TStringView suffix(line.data() + prefixLength, line.size() - prefixLength);
                            if (!suffix.empty())
                            {
                                const int prefixWidth = strwidth(prefix);
                                buffer.moveStr(prefixWidth, suffix, normal);
                            }
                        }
                        else
                        {
                            const TColorAttr attr = isHighlighted ? highlight : normal;
                            buffer.moveStr(0, TStringView(m_lines[y]), attr);
                        }
                    }
                    writeLine(0, y, size.x, 1, buffer);
                }
            }

        private:
            std::vector<std::string> m_lines;
            size_t m_highlightIndex;
            size_t m_highlightPrefixLength;
        };

        inline std::vector<std::string> splitLinesPreservingEmpties(const std::string &text)
        {
            std::vector<std::string> lines;
            std::string current;
            for (char ch : text)
            {
                if (ch == '\n')
                {
                    lines.emplace_back(std::move(current));
                    current.clear();
                }
                else
                {
                    current.push_back(ch);
                }
            }
            lines.emplace_back(std::move(current));
            return lines;
        }

        inline size_t findFirstNonEmptyLine(const std::vector<std::string> &lines)
        {
            for (size_t index = 0; index < lines.size(); ++index)
                if (!lines[index].empty())
                    return index;
            return 0;
        }

        inline int computeMaxLineWidth(const std::vector<std::string> &lines)
        {
            int maxWidth = 0;
            for (const auto &line : lines)
                maxWidth = std::max(maxWidth, strwidth(TStringView(line)));
            return maxWidth;
        }

    } // namespace detail

    inline void showAboutDialog(const AboutDialogInfo &info)
    {
        const std::string message = buildAboutDialogMessage(info);
        auto lines = detail::splitLinesPreservingEmpties(message);
        const size_t highlightIndex = detail::findFirstNonEmptyLine(lines);
        size_t highlightPrefixLength = 0;
        if (highlightIndex < lines.size() && !info.applicationName.empty())
        {
            const std::string &line = lines[highlightIndex];
            if (line.compare(0, info.applicationName.size(), info.applicationName) == 0)
                highlightPrefixLength = info.applicationName.size();
        }
        const int maxLineWidth = detail::computeMaxLineWidth(lines);

        constexpr int kMinWidth = 40;
        constexpr int kMinHeight = 9;
        const int textHeight = static_cast<int>(lines.size());
        const int dialogWidth = std::max(kMinWidth, maxLineWidth + 5);
        const int dialogHeight = std::max(kMinHeight, textHeight + 6);

        TRect bounds(0, 0, dialogWidth, dialogHeight);
        bounds.move((TProgram::deskTop->size.x - dialogWidth) / 2,
                    (TProgram::deskTop->size.y - dialogHeight) / 2);

        TDialog *dialog = new TDialog(bounds, MsgBoxText::informationText);

        const TRect textBounds(3, 2, dialog->size.x - 2, dialog->size.y - 3);
        dialog->insert(new detail::AboutStaticText(textBounds,
                                                   message,
                                                   std::move(lines),
                                                   highlightIndex,
                                                   highlightPrefixLength));

        TButton *okButton = new TButton(TRect(0, 0, 10, 2), MsgBoxText::okText, cmOK, bfDefault);
        dialog->insert(okButton);
        const int buttonX = (dialog->size.x - okButton->size.x) / 2;
        okButton->moveTo(buttonX, dialog->size.y - 3);

        dialog->selectNext(False);
        TProgram::application->execView(dialog);
        TObject::destroy(dialog);
    }

    inline void showAboutDialog(std::string_view toolName,
                                std::string_view version,
                                std::string_view description)
    {
        showAboutDialog(AboutDialogInfo{toolName, version, description});
    }

} // namespace ck::ui
