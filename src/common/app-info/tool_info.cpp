#include "ck/app_info.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace ck::appinfo
{
    namespace
    {

        constexpr std::array<ToolInfo, 5> kTools{{
            ToolInfo{
                "ck-utilities",
                "ck-utilities",
                "CK Utilities",
                "Launch CK tools from a unified Turbo Vision shell.",
                "Launch CK tools from a unified Turbo Vision shell.",
                "CK Utilities is the landing pad for the suite. It presents every installed tool with rich descriptions, a consistent launch workflow, and shortcuts for discovery. Use it as a starting point in new terminals to remind yourself of capabilities and jump straight into the utility you need."},
            ToolInfo{
                "ck-edit",
                "ck-edit",
                "Edit",
                "Edit text and Markdown documents with live structural hints.",
                "Edit text and Markdown documents with live structural hints.",
                "Edit keeps Markdown editing fast inside the terminal. It pairs a Turbo Vision interface with helpers for headings, lists, and formatting so you stay in flow. Use it for quick note taking, documentation tweaks, or reviewing rendered structure without leaving your shell."},
            ToolInfo{
                "ck-du",
                "ck-du",
                "Disk Usage",
                "Analyze directory and file storage utilization.",
                "Analyze directory and file storage utilization.",
                "Disk Usage visualizes disk usage with an ncdu-inspired tree and rich metadata. Open multiple windows to compare paths, switch units on the fly, and inspect recursive file listings with owners, timestamps, and filters. It is built to answer “where did my space go?” without memorizing long du pipelines."},
            ToolInfo{
                "ck-json-view",
                "ck-json-view",
                "JSON View",
                "Inspect and navigate JSON documents interactively.",
                "Inspect and navigate JSON documents interactively.",
                "JSON View parses JSON into a navigable tree with keyboard-first controls. Expand nodes to reveal structured previews, search across the document with highlighted matches, and copy selections using OSC 52 when your terminal supports it. It is ideal for exploring API responses or configuration blobs in a readable form."},
            ToolInfo{
                "ck-config",
                "ck-config",
                "Config",
                "Manage ck-utilities configuration defaults.",
                "Manage ck-utilities configuration defaults.",
                "Config centralizes application defaults for every CK utility. Browse known apps, tweak options with validation, and export or import profiles for teammates. It keeps environment-wide settings—like ignore patterns or display preferences—consistent without hunting through dotfiles."},
        }};

    } // namespace

    std::span<const ToolInfo> tools() noexcept
    {
        return std::span<const ToolInfo>{kTools};
    }

    const ToolInfo *findTool(std::string_view id) noexcept
    {
        auto it = std::find_if(kTools.begin(), kTools.end(), [&](const ToolInfo &info)
                               { return info.id == id; });
        if (it == kTools.end())
            return nullptr;
        return &*it;
    }

    const ToolInfo &requireTool(std::string_view id)
    {
        if (const ToolInfo *info = findTool(id))
            return *info;
        throw std::runtime_error("Unknown tool id: " + std::string{id});
    }

    const ToolInfo *findToolByExecutable(std::string_view executable) noexcept
    {
        auto it = std::find_if(kTools.begin(), kTools.end(), [&](const ToolInfo &info)
                               { return info.executable == executable; });
        if (it == kTools.end())
            return nullptr;
        return &*it;
    }

    const ToolInfo &requireToolByExecutable(std::string_view executable)
    {
        if (const ToolInfo *info = findToolByExecutable(executable))
            return *info;
        throw std::runtime_error("Unknown tool executable: " + std::string{executable});
    }

} // namespace ck::appinfo
