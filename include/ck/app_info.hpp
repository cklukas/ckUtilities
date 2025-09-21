#pragma once

#include <span>
#include <string_view>

namespace ck::appinfo
{

struct ToolInfo
{
    std::string_view id;
    std::string_view executable;
    std::string_view displayName;
    std::string_view shortDescription;
    std::string_view aboutDescription;
    std::string_view longDescription;
};

std::span<const ToolInfo> tools() noexcept;

const ToolInfo *findTool(std::string_view id) noexcept;

const ToolInfo &requireTool(std::string_view id);

const ToolInfo *findToolByExecutable(std::string_view executable) noexcept;

const ToolInfo &requireToolByExecutable(std::string_view executable);

inline constexpr std::string_view kProjectBanner = R"( ██████╗██╗  ██╗    ██╗   ██╗████████╗██╗██╗     ██╗████████╗██╗███████╗███████╗
██╔════╝██║ ██╔╝    ██║   ██║╚══██╔══╝██║██║     ██║╚══██╔══╝██║██╔════╝██╔════╝
██║     █████╔╝     ██║   ██║   ██║   ██║██║     ██║   ██║   ██║█████╗  ███████╗
██║     ██╔═██╗     ██║   ██║   ██║   ██║██║     ██║   ██║   ██║██╔══╝  ╚════██║
╚██████╗██║  ██╗    ╚██████╔╝   ██║   ██║███████╗██║   ██║   ██║███████╗███████║
 ╚═════╝╚═╝  ╚═╝     ╚═════╝    ╚═╝   ╚═╝╚══════╝╚═╝   ╚═╝   ╚═╝╚══════╝╚══════╝
                                                                                 )";

} // namespace ck::appinfo

