#include "ck/hotkeys.hpp"

#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>

#include "ck/options.hpp"
#include <nlohmann/json.hpp>

#include <tvision/util.h>

namespace ck::hotkeys
{

namespace
{

struct SchemeData
{
    std::string id;
    std::string displayName;
    std::string description;
    std::unordered_map<std::uint16_t, KeyBinding> bindings;
};

std::vector<SchemeData> gSchemes;

struct ActiveScheme
{
    SchemeData *scheme = nullptr;
};

ActiveScheme gActive;
std::string gActiveId;

std::unordered_map<std::string, std::unordered_map<std::uint16_t, std::string>> gLabelsByLocale;
std::string gActiveLocale = "en";
std::unordered_map<std::uint16_t, std::string> gCommandTools;

SchemeData *findScheme(std::string_view id)
{
    auto it = std::find_if(gSchemes.begin(), gSchemes.end(), [&](const SchemeData &scheme) {
        return scheme.id == id;
    });
    return it != gSchemes.end() ? &*it : nullptr;
}

SchemeData &ensureScheme(std::string_view id)
{
    if (SchemeData *existing = findScheme(id))
        return *existing;
    SchemeData data;
    data.id = std::string(id);
    data.displayName = std::string(id);
    data.description.clear();
    gSchemes.push_back(std::move(data));
    return gSchemes.back();
}

void upsertBinding(SchemeData &scheme, const KeyBinding &binding)
{
    if (binding.command == 0)
        return;
    scheme.bindings[binding.command] = binding;
}

void ensureActiveScheme()
{
    if (gActive.scheme || gSchemes.empty())
        return;
    gActive.scheme = &gSchemes.front();
    gActiveId = gActive.scheme->id;
}

const std::string *findLabel(const std::string &locale, std::uint16_t command)
{
    auto itLocale = gLabelsByLocale.find(locale);
    if (itLocale == gLabelsByLocale.end())
        return nullptr;
    auto it = itLocale->second.find(command);
    if (it == itLocale->second.end())
        return nullptr;
    return &it->second;
}

} // namespace

void registerSchemes(std::span<const Scheme> schemes)
{
    for (const auto &scheme : schemes)
    {
        SchemeData &data = ensureScheme(scheme.id);
        if (!scheme.displayName.empty())
            data.displayName = scheme.displayName;
        if (!scheme.description.empty())
            data.description = scheme.description;
        for (const auto &binding : scheme.bindings)
            upsertBinding(data, binding);
    }
    ensureActiveScheme();
}

void setActiveScheme(std::string_view id)
{
    if (id.empty())
        return;
    if (gActive.scheme && gActive.scheme->id == id)
        return;
    if (SchemeData *scheme = findScheme(id))
    {
        gActive.scheme = scheme;
        gActiveId = scheme->id;
    }
}

std::string_view activeScheme()
{
    return gActiveId;
}

const KeyBinding *lookup(std::uint16_t command) noexcept
{
    if (!gActive.scheme)
        return nullptr;
    if (auto it = gActive.scheme->bindings.find(command); it != gActive.scheme->bindings.end())
        return &it->second;
    return nullptr;
}

TKey key(std::uint16_t command) noexcept
{
    if (const auto *binding = lookup(command))
        return binding->key;
    return {};
}

std::string displayText(std::uint16_t command)
{
    if (const auto *binding = lookup(command))
        return binding->display;
    return {};
}

std::string statusLabel(std::uint16_t command, std::string_view action)
{
    const auto *binding = lookup(command);
    if (!binding || binding->display.empty())
        return std::string(action);

    std::string label;
    label.reserve(binding->display.size() + action.size() + 4);
    label.append("~");
    label.append(binding->display);
    label.append("~ ");
    label.append(action);
    return label;
}

void configureMenuItem(TMenuItem &item)
{
    if (!item.command)
        return;
    if (const auto *binding = lookup(item.command))
    {
        item.keyCode = binding->key;
        if (!binding->display.empty())
        {
            delete[] (char *)item.param;
            item.param = newStr(binding->display.c_str());
        }
    }
}

void configureMenuTree(TMenuItem &root)
{
    for (TMenuItem *item = &root; item; item = item->next)
    {
        if (item->command)
            configureMenuItem(*item);
        if (!item->command && item->subMenu)
        {
            if (item->subMenu->items)
                configureMenuTree(*item->subMenu->items);
        }
    }
}

void configureStatusItem(TStatusItem &item, std::string_view action)
{
    if (const auto *binding = lookup(item.command))
    {
        item.keyCode = binding->key;
        if (!binding->display.empty())
        {
            auto label = statusLabel(item.command, action);
            delete[] item.text;
            item.text = newStr(label.c_str());
        }
    }
}

void initializeFromEnvironment()
{
    if (const char *scheme = std::getenv("CK_HOTKEY_SCHEME"))
        setActiveScheme(scheme);
}

void applyCommandLineScheme(int &argc, char **argv)
{
    int writeIndex = 1;
    for (int readIndex = 1; readIndex < argc; ++readIndex)
    {
        std::string_view arg(argv[readIndex]);
        if (arg.rfind("--hotkeys=", 0) == 0)
        {
            setActiveScheme(arg.substr(10));
            continue;
        }
        if (arg == "--hotkeys")
        {
            if (readIndex + 1 < argc)
            {
                setActiveScheme(argv[readIndex + 1]);
                ++readIndex;
            }
            continue;
        }
        argv[writeIndex++] = argv[readIndex];
    }
    argc = writeIndex;
    if (argv)
        argv[writeIndex] = nullptr;
}

void registerCommandLabels(std::span<const CommandLabel> labels, std::string_view locale)
{
    auto &map = gLabelsByLocale[std::string(locale)];
    for (const auto &entry : labels)
    {
        if (entry.command == 0)
            continue;
        map[entry.command] = entry.label;
        if (!entry.toolId.empty())
            gCommandTools[entry.command] = std::string(entry.toolId);
        else if (!gCommandTools.count(entry.command))
            gCommandTools[entry.command] = std::string{};
    }
}

std::string commandLabel(std::uint16_t command)
{
    if (const std::string *label = findLabel(gActiveLocale, command))
        return *label;
    if (gActiveLocale != "en")
        if (const std::string *label = findLabel("en", command))
            return *label;
    return {};
}

void setLocale(std::string_view locale)
{
    if (locale.empty())
        return;
    gActiveLocale.assign(locale);
}

std::string_view activeLocale()
{
    return gActiveLocale;
}

std::vector<std::uint16_t> commandsForTool(std::string_view toolId)
{
    std::vector<std::uint16_t> commands;
    for (const auto &[command, owner] : gCommandTools)
    {
        if (owner == toolId)
            commands.push_back(command);
    }
    std::sort(commands.begin(), commands.end());
    return commands;
}

std::vector<std::uint16_t> allCommands()
{
    std::vector<std::uint16_t> commands;
    commands.reserve(gCommandTools.size());
    for (const auto &[command, _] : gCommandTools)
        commands.push_back(command);
    std::sort(commands.begin(), commands.end());
    return commands;
}

std::vector<KeyBinding> schemeBindings(std::string_view schemeId)
{
    std::vector<KeyBinding> result;
    if (SchemeData *scheme = findScheme(schemeId))
    {
        result.reserve(scheme->bindings.size());
        for (const auto &[command, binding] : scheme->bindings)
            result.push_back(binding);
        std::sort(result.begin(), result.end(), [](const KeyBinding &a, const KeyBinding &b) {
            return a.command < b.command;
        });
    }
    return result;
}

std::string formatKey(TKey key)
{
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    if (key.mods & kbCtrlShift)
        ctrl = true;
    if (key.mods & kbAltShift)
        alt = true;
    if (key.mods & kbShift)
        shift = true;

    ushort code = key.code;
    std::string base;

    auto setFunctionKey = [&](ushort origin, ushort value, bool &flag, bool flagValue) -> bool {
        if (code >= origin && code <= origin + 11)
        {
            flag = flagValue || flag;
            base = "F" + std::to_string(static_cast<int>(code - origin + 1));
            code = 0;
            return true;
        }
        return false;
    };

    if (code >= kbF1 && code <= kbF12)
    {
        base = "F" + std::to_string(static_cast<int>(code - kbF1 + 1));
        code = 0;
    }
    else if (setFunctionKey(kbShiftF1, code, shift, true))
        ;
    else if (setFunctionKey(kbCtrlF1, code, ctrl, true))
        ;
    else if (setFunctionKey(kbAltF1, code, alt, true))
        ;
    else if (code == kbCtrlEnter)
    {
        ctrl = true;
        base = "Enter";
        code = 0;
    }
    else if (code == kbEnter)
    {
        base = "Enter";
        code = 0;
    }
    else if (code == kbEsc)
    {
        base = "Esc";
        code = 0;
    }
    else if (code == kbTab)
    {
        base = "Tab";
        code = 0;
    }
    else if (code == kbBack)
    {
        base = "Backspace";
        code = 0;
    }
    else if (code == kbDel)
    {
        base = "Del";
        code = 0;
    }
    else if (code == kbIns)
    {
        base = "Ins";
        code = 0;
    }
    else if (code == kbLeft)
    {
        base = "Left";
        code = 0;
    }
    else if (code == kbRight)
    {
        base = "Right";
        code = 0;
    }
    else if (code == kbUp)
    {
        base = "Up";
        code = 0;
    }
    else if (code == kbDown)
    {
        base = "Down";
        code = 0;
    }
    else if (code >= kbAlt1 && code <= kbAlt9)
    {
        alt = true;
        base = std::to_string(static_cast<int>(code - kbAlt1 + 1));
        code = 0;
    }
    else if (code == kbAlt0)
    {
        alt = true;
        base = "0";
        code = 0;
    }
    else if (code >= kbAltA && code <= kbAltZ)
    {
        alt = true;
        base.assign(1, static_cast<char>('A' + (code - kbAltA)));
        code = 0;
    }

    if (code != 0 && code >= 32 && code < 127)
    {
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(code)));
        base.assign(1, ch);
        code = 0;
    }

    if (base.empty() && code != 0)
    {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << code;
        base = oss.str();
    }

    std::vector<std::string> parts;
    if (ctrl)
        parts.emplace_back("Ctrl");
    if (alt)
        parts.emplace_back("Alt");
    if (shift)
        parts.emplace_back("Shift");
    parts.push_back(base.empty() ? std::string("Unknown") : base);

    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out << '+';
        out << parts[i];
    }
    return out.str();
}

void setBinding(std::string_view schemeId, std::uint16_t command, TKey key, std::string display)
{
    SchemeData &scheme = ensureScheme(schemeId);
    if (display.empty())
        display = formatKey(key);
    KeyBinding binding{command, key, display};
    upsertBinding(scheme, binding);
}

void clearBinding(std::string_view schemeId, std::uint16_t command)
{
    if (SchemeData *scheme = findScheme(schemeId))
    {
        scheme->bindings.erase(command);
    }
}

void registerDefaultSchemes()
{
    // Implemented in default_schemes.cpp.
    extern void registerBuiltinHotkeySchemes();
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    registerBuiltinHotkeySchemes();
    ensureActiveScheme();
}

} // namespace ck::hotkeys
