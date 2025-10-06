#include "ck/hotkeys.hpp"

#include "ck/commands/ck_chat.hpp"
#include "ck/commands/ck_config.hpp"
#include "ck/commands/ck_du.hpp"
#include "ck/commands/ck_find.hpp"
#include "ck/commands/ck_utilities.hpp"
#include "ck/commands/json_view.hpp"

#include <tvision/views.h>
#include <tvision/tkeys.h>

namespace ck::hotkeys
{

namespace
{

const KeyBinding kDefaultBindings[] = {
    {cmQuit, TKey(kbAltX), "Alt-X"},
    {cmClose, TKey(kbAltF3), "Alt-F3"},
    {cmMenu, TKey(kbF10), "F10"},
    {cmZoom, TKey(kbF5), "F5"},
    {cmResize, TKey(kbCtrlF5), "Ctrl-F5"},
    {cmNext, TKey(kbF6), "F6"},
    {cmOpen, TKey(kbF2), "F2"},
    {cmCopy, TKey(kbCtrlC), "Ctrl-C"},
    {cmFind, TKey(kbCtrlF), "Ctrl-F"},
    {cmHelp, TKey(kbF1), "F1"},

    {commands::utilities::LaunchTool, TKey(kbEnter), "Enter"},
    {commands::utilities::ToggleEventViewer, TKey(kbAlt0), "Alt-0"},

    {commands::disk_usage::ViewFiles, TKey(kbF3), "F3"},
    {commands::disk_usage::ViewFilesRecursive, TKey(kbShiftF3), "Shift-F3"},
    {commands::disk_usage::ViewFileTypes, TKey(kbF4), "F4"},
    {commands::disk_usage::ViewFileTypesRecursive, TKey(kbShiftF4), "Shift-F4"},
    {commands::disk_usage::SortNameAsc, TKey(kbCtrlN), "Ctrl-N"},
    {commands::disk_usage::SortSizeDesc, TKey(kbCtrlS), "Ctrl-S"},
    {commands::disk_usage::SortModifiedDesc, TKey(kbCtrlM), "Ctrl-M"},
    {commands::disk_usage::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::disk_usage::About, TKey(kbF1), "F1"},

    {commands::config::ReloadApps, TKey(kbF5), "F5"},
    {commands::config::EditApp, TKey(kbEnter), "Enter"},
    {commands::config::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::config::About, TKey(kbF1), "F1"},

    {commands::find::NewSearch, TKey(kbF2), "F2"},
    {commands::find::LoadSpec, TKey(kbCtrlO), "Ctrl-O"},
    {commands::find::SaveSpec, TKey(kbCtrlS), "Ctrl-S"},
    {commands::find::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::find::About, TKey(kbF1), "F1"},

    {commands::json_view::Find, TKey(kbCtrlF), "Ctrl-F"},
    {commands::json_view::FindNext, TKey(kbF5), "F5"},
    {commands::json_view::FindPrev, TKey(kbShiftF5), "Shift-F5"},
    {commands::json_view::EndSearch, TKey(kbEsc), "Esc"},
    {commands::json_view::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::json_view::Level0, TKey(kbAlt0), "Alt-0"},
    {commands::json_view::Level1, TKey(kbAlt1), "Alt-1"},
    {commands::json_view::Level2, TKey(kbAlt2), "Alt-2"},
    {commands::json_view::Level3, TKey(kbAlt3), "Alt-3"},
    {commands::json_view::Level4, TKey(kbAlt4), "Alt-4"},
    {commands::json_view::Level5, TKey(kbAlt5), "Alt-5"},
    {commands::json_view::Level6, TKey(kbAlt6), "Alt-6"},
    {commands::json_view::Level7, TKey(kbAlt7), "Alt-7"},
    {commands::json_view::Level8, TKey(kbAlt8), "Alt-8"},
    {commands::json_view::Level9, TKey(kbAlt9), "Alt-9"},

    {commands::chat::NewChat, TKey(kbCtrlN), "Ctrl-N"},
    {commands::chat::ManageModels, TKey(kbF2), "F2"},
    {commands::chat::ManagePrompts, TKey(kbF3), "F3"},
    {commands::chat::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::chat::SendPrompt, TKey(kbAltS), "Alt-S"},
    {commands::chat::About, TKey(kbF1), "F1"},
};

const KeyBinding kMacBindings[] = {
    {cmQuit, TKey(kbCtrlQ), "Ctrl-Q"},
    {cmClose, TKey(kbCtrlW), "Ctrl-W"},
    {cmMenu, TKey(kbF10), "F10"},
    {cmZoom, TKey(kbF5), "F5"},
    {cmResize, TKey(kbCtrlF5), "Ctrl-F5"},
    {cmNext, TKey(kbF6), "F6"},
    {cmOpen, TKey(kbCtrlO), "Ctrl-O"},
    {cmCopy, TKey(kbCtrlC), "Ctrl-C"},
    {cmFind, TKey(kbCtrlF), "Ctrl-F"},
    {cmHelp, TKey(kbF1), "F1"},

    {commands::utilities::LaunchTool, TKey(kbEnter), "Enter"},
    {commands::utilities::ToggleEventViewer, TKey('0', kbCtrlShift), "Ctrl-0"},

    {commands::disk_usage::ViewFiles, TKey(kbF3), "F3"},
    {commands::disk_usage::ViewFilesRecursive, TKey(kbShiftF3), "Shift-F3"},
    {commands::disk_usage::ViewFileTypes, TKey(kbF4), "F4"},
    {commands::disk_usage::ViewFileTypesRecursive, TKey(kbShiftF4), "Shift-F4"},
    {commands::disk_usage::SortNameAsc, TKey(kbCtrlN), "Ctrl-N"},
    {commands::disk_usage::SortSizeDesc, TKey(kbCtrlS), "Ctrl-S"},
    {commands::disk_usage::SortModifiedDesc, TKey(kbCtrlM), "Ctrl-M"},
    {commands::disk_usage::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::disk_usage::About, TKey(kbF1), "F1"},

    {commands::config::ReloadApps, TKey(kbF5), "F5"},
    {commands::config::EditApp, TKey(kbEnter), "Enter"},
    {commands::config::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::config::About, TKey(kbF1), "F1"},

    {commands::find::NewSearch, TKey(kbF2), "F2"},
    {commands::find::LoadSpec, TKey(kbCtrlO), "Ctrl-O"},
    {commands::find::SaveSpec, TKey(kbCtrlS), "Ctrl-S"},
    {commands::find::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::find::About, TKey(kbF1), "F1"},

    {commands::json_view::Find, TKey(kbCtrlF), "Ctrl-F"},
    {commands::json_view::FindNext, TKey(kbF5), "F5"},
    {commands::json_view::FindPrev, TKey(kbShiftF5), "Shift-F5"},
    {commands::json_view::EndSearch, TKey(kbEsc), "Esc"},
    {commands::json_view::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::json_view::Level0, TKey('0', kbCtrlShift), "Ctrl-0"},
    {commands::json_view::Level1, TKey('1', kbCtrlShift), "Ctrl-1"},
    {commands::json_view::Level2, TKey('2', kbCtrlShift), "Ctrl-2"},
    {commands::json_view::Level3, TKey('3', kbCtrlShift), "Ctrl-3"},
    {commands::json_view::Level4, TKey('4', kbCtrlShift), "Ctrl-4"},
    {commands::json_view::Level5, TKey('5', kbCtrlShift), "Ctrl-5"},
    {commands::json_view::Level6, TKey('6', kbCtrlShift), "Ctrl-6"},
    {commands::json_view::Level7, TKey('7', kbCtrlShift), "Ctrl-7"},
    {commands::json_view::Level8, TKey('8', kbCtrlShift), "Ctrl-8"},
    {commands::json_view::Level9, TKey('9', kbCtrlShift), "Ctrl-9"},

    {commands::chat::NewChat, TKey(kbCtrlN), "Ctrl-N"},
    {commands::chat::ManageModels, TKey(kbF2), "F2"},
    {commands::chat::ManagePrompts, TKey(kbF3), "F3"},
    {commands::chat::ReturnToLauncher, TKey(kbCtrlL), "Ctrl-L"},
    {commands::chat::SendPrompt, TKey(kbCtrlEnter), "Ctrl-Enter"},
    {commands::chat::About, TKey(kbF1), "F1"},
};

const Scheme kBuiltInSchemes[] = {
    {"default", "Default", "Standard Turbo Vision shortcuts", kDefaultBindings},
    {"mac", "macOS", "Mac-friendly shortcuts without Alt combos", kMacBindings},
};

} // namespace

void registerBuiltinHotkeySchemes()
{
    registerSchemes(kBuiltInSchemes);
#if defined(__APPLE__)
    setActiveScheme("mac");
#else
    setActiveScheme("default");
#endif
}

} // namespace ck::hotkeys
