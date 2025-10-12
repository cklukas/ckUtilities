#include "ck/hotkeys.hpp"

#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iomanip>

#include "ck/options.hpp"
#include <nlohmann/json.hpp>

#include <tvision/util.h>

namespace nlohmann
{
template <>
struct adl_serializer<TKey>
{
    static void to_json(json &j, const TKey &key)
    {
        j = json{{"code", key.code}, {"mods", key.mods}};
    }

    static void from_json(const json &j, TKey &key)
    {
        j.at("code").get_to(key.code);
        j.at("mods").get_to(key.mods);
    }
};
} // namespace nlohmann

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

constexpr std::string_view kCustomSchemeId = "custom";
constexpr std::string_view kAutoSchemeId = "auto";

std::string platformDefaultSchemeId()
{
#ifdef __APPLE__
    return "mac";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

bool gConfigLoaded = false;
bool gConfigDirty = false;
bool gCustomDirty = false;
bool gRuntimeOverride = false;
bool gHasCustom = false;
std::string gPreferredScheme = std::string(kAutoSchemeId);
std::string gCustomBase = platformDefaultSchemeId();

std::filesystem::path configFilePath()
{
    if (const char *overridePath = std::getenv("CK_HOTKEYS_CONFIG"))
    {
        if (overridePath[0] != '\0')
            return std::filesystem::path(overridePath);
    }
    return ck::config::OptionRegistry::configRoot() / "hotkeys.json";
}

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

SchemeData &ensureCustomScheme()
{
    return ensureScheme(kCustomSchemeId);
}

void saveConfiguration()
{
    if (!gConfigDirty && !gCustomDirty)
        return;

    nlohmann::json j;
    j["preferred_scheme"] = gPreferredScheme;

    if (SchemeData* customScheme = findScheme(kCustomSchemeId))
    {
        j["custom_scheme_base"] = gCustomBase;
        nlohmann::json bindings = nlohmann::json::object();
        for (const auto& [command, binding] : customScheme->bindings)
        {
            nlohmann::json b;
            b["key"] = binding.key;
            b["display"] = binding.display;
            bindings[std::to_string(command)] = b;
        }
        j["custom_scheme_bindings"] = bindings;
    }

    try
    {
        std::ofstream file(configFilePath());
        file << std::setw(4) << j << std::endl;
        gConfigDirty = false;
        gCustomDirty = false;
    }
    catch (const std::exception& e)
    {
        // TODO: log error
    }
}

void loadConfiguration()
{
    gConfigLoaded = true;
    std::filesystem::path path = configFilePath();
    if (!std::filesystem::exists(path))
        return;

    try
    {
        std::ifstream file(path);
        nlohmann::json j;
        file >> j;

        if (j.contains("preferred_scheme"))
            gPreferredScheme = j["preferred_scheme"].get<std::string>();

        if (j.contains("custom_scheme_bindings"))
        {
            gHasCustom = true;
            if (j.contains("custom_scheme_base"))
                gCustomBase = j["custom_scheme_base"].get<std::string>();

            SchemeData& customScheme = ensureCustomScheme();
            customScheme.displayName = "Custom";
            customScheme.description = "User-defined hotkey scheme";

            const nlohmann::json& bindings = j["custom_scheme_bindings"];
            for (auto it = bindings.begin(); it != bindings.end(); ++it)
            {
                try
                {
                    std::uint16_t command = std::stoul(it.key());
                    const nlohmann::json& b = it.value();
                    KeyBinding binding;
                    binding.command = command;
                    binding.key = b["key"].get<TKey>();
                    binding.display = b["display"].get<std::string>();
                    upsertBinding(customScheme, binding);
                }
                catch (const std::exception&)
                {
                    // Ignore malformed entries
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        // TODO: log error
    }
}

void applyPreferredScheme()
{
    std::string schemeId = gPreferredScheme;
    if (schemeId == kAutoSchemeId)
    {
        schemeId = platformDefaultSchemeId();
    }
    else if (schemeId == kCustomSchemeId && !gHasCustom)
    {
        schemeId = platformDefaultSchemeId();
    }

    setActiveScheme(schemeId);
}


void ensureConfigurationLoaded()
{
    if (!gConfigLoaded)
        loadConfiguration();
}

KeyBinding normalizeBinding(const KeyBinding &binding)
{
    KeyBinding normalized = binding;
    if (normalized.display.empty())
        normalized.display = formatKey(normalized.key);
    return normalized;
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

void init()
{
    registerDefaultSchemes();
    ensureConfigurationLoaded();
    applyPreferredScheme();
}

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
        item.keyCode = binding->key.code;
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
        item.keyCode = binding->key.code;
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
            gRuntimeOverride = true;
            setActiveScheme(arg.substr(10));
            continue;
        }
        if (arg == "--hotkeys")
        {
            if (readIndex + 1 < argc)
            {
                gRuntimeOverride = true;
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
        base.assign(1, 'A' + (code - kbAltA));
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
    if (schemeId == kCustomSchemeId)
    {
        gCustomDirty = true;
        saveConfiguration();
    }
}

void clearBinding(std::string_view schemeId, std::uint16_t command)
{
    if (SchemeData *scheme = findScheme(schemeId))
    {
        scheme->bindings.erase(command);
        if (schemeId == kCustomSchemeId)
        {
            gCustomDirty = true;
            saveConfiguration();
        }
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

std::string defaultSchemeId()
{
    return platformDefaultSchemeId();
}

std::string preferredScheme()
{
    ensureConfigurationLoaded();
    return gPreferredScheme;
}

void setPreferredScheme(std::string_view id)
{
    if (gRuntimeOverride) return;
    ensureConfigurationLoaded();
    std::string newScheme(id);
    if (gPreferredScheme == newScheme)
        return;
    gPreferredScheme = newScheme;
    gConfigDirty = true;
    saveConfiguration();
    applyPreferredScheme();
}

bool customSchemeExists()
{
    ensureConfigurationLoaded();
    return gHasCustom;
}

std::string customBaseScheme()
{
    ensureConfigurationLoaded();
    return gCustomBase;
}

bool createCustomScheme(std::string_view templateId)
{
    ensureConfigurationLoaded();
    if (gHasCustom)
        return true;
    SchemeData* templateScheme = findScheme(templateId);
    if (!templateScheme)
        return false;

    gCustomBase = templateScheme->id;
    SchemeData& customScheme = ensureCustomScheme();
    customScheme.bindings = templateScheme->bindings;
    gHasCustom = true;
    gCustomDirty = true;
    saveConfiguration();
    return true;
}

void clearCustomScheme()
{
    ensureConfigurationLoaded();
    if (!gHasCustom) return;
    if (SchemeData* customScheme = findScheme(kCustomSchemeId))
    {
        customScheme->bindings.clear();
    }
    gHasCustom = false;
    gCustomDirty = true;
    saveConfiguration();
    if (gPreferredScheme == kCustomSchemeId)
    {
        applyPreferredScheme();
    }
}

std::string commandTool(std::uint16_t command)
{
    auto it = gCommandTools.find(command);
    if (it != gCommandTools.end())
        return it->second;
    return {};
}

std::vector<std::pair<std::string, std::string>> availableSchemes()
{
    ensureConfigurationLoaded();
    std::vector<std::pair<std::string, std::string>> result;
    result.emplace_back(kAutoSchemeId, "Auto");
    for (const auto& scheme : gSchemes)
    {
        if (scheme.id == kCustomSchemeId) continue;
        result.emplace_back(scheme.id, scheme.displayName);
    }
    if (gHasCustom)
    {
        result.emplace_back(kCustomSchemeId, findScheme(kCustomSchemeId)->displayName);
    }
    return result;
}

void setCustomBindings(std::span<const KeyBinding> bindings, bool markDirty)
{
    if (!gHasCustom) return;
    SchemeData& scheme = ensureCustomScheme();
    scheme.bindings.clear();
    for(const auto& binding : bindings)
    {
        upsertBinding(scheme, binding);
    }
    if (markDirty)
    {
        gCustomDirty = true;
    }
}

void saveCustomScheme()
{
    if (gCustomDirty)
    {
        saveConfiguration();
    }
}

} // namespace ck::hotkeys