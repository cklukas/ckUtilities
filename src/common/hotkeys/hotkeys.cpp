#include "ck/hotkeys.hpp"

#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>

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

SchemeData *findScheme(std::string_view id)
{
    auto it = std::find_if(gSchemes.begin(), gSchemes.end(), [&](const SchemeData &scheme) {
        return scheme.id == id;
    });
    return it != gSchemes.end() ? &*it : nullptr;
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

} // namespace

void registerSchemes(std::span<const Scheme> schemes)
{
    for (const auto &scheme : schemes)
    {
        SchemeData *data = findScheme(scheme.id);
        if (!data)
        {
            SchemeData newData;
            newData.id = std::string(scheme.id);
            newData.displayName = std::string(scheme.displayName);
            newData.description = std::string(scheme.description);
            data = &gSchemes.emplace_back(std::move(newData));
        }
        else
        {
            if (!scheme.displayName.empty())
                data->displayName = scheme.displayName;
            if (!scheme.description.empty())
                data->description = scheme.description;
        }
        for (const auto &binding : scheme.bindings)
            upsertBinding(*data, binding);
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
