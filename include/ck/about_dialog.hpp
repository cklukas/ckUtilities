#pragma once

#ifndef Uses_MsgBox
#define CK_ABOUT_DIALOG_DEFINE_USES_MSGBOX
#define Uses_MsgBox
#endif
#include <tvision/tv.h>
#ifdef CK_ABOUT_DIALOG_DEFINE_USES_MSGBOX
#undef Uses_MsgBox
#undef CK_ABOUT_DIALOG_DEFINE_USES_MSGBOX
#endif

#include <sstream>
#include <string>
#include <string_view>

namespace ck::ui
{
struct AboutDialogInfo
{
    std::string_view toolName;
    std::string_view version;
    std::string_view description;
    std::string_view developer = "Dr. C. Klukas";
    std::string_view copyright = "(c) 2025 by Dr. C. Klukas";
    std::string_view applicationName = "CK Utilities";
    std::string_view buildDate = __DATE__;
    std::string_view buildTime = __TIME__;
};

inline std::string buildAboutDialogMessage(const AboutDialogInfo &info)
{
    std::ostringstream out;
    out << info.applicationName;

    out << "\n\n" << info.toolName;
    if (!info.description.empty())
        out << "\n" << info.description;

    if (!info.version.empty())
        out << "\nVersion: " << info.version;

    if (!info.buildDate.empty())
    {
        out << "\nBuild: " << info.buildDate;
        if (!info.buildTime.empty())
            out << ' ' << info.buildTime;
    }

    out << "\n\nDeveloper: " << info.developer;
    if (!info.copyright.empty())
        out << "\n" << info.copyright;

    return out.str();
}

inline void showAboutDialog(const AboutDialogInfo &info)
{
    const std::string message = buildAboutDialogMessage(info);
    messageBox(message.c_str(), mfInformation | mfOKButton);
}

inline void showAboutDialog(std::string_view toolName,
                            std::string_view version,
                            std::string_view description)
{
    showAboutDialog(AboutDialogInfo{toolName, version, description});
}

} // namespace ck::ui
