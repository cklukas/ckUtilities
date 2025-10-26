#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef Uses_ipstream
#define Uses_ipstream
#endif
#ifndef Uses_opstream
#define Uses_opstream
#endif
#ifndef Uses_TKeys
#define Uses_TKeys
#endif
#ifndef Uses_TMenu
#define Uses_TMenu
#endif
#ifndef Uses_TMenuItem
#define Uses_TMenuItem
#endif
#ifndef Uses_TSubMenu
#define Uses_TSubMenu
#endif
#ifndef Uses_TStatusLine
#define Uses_TStatusLine
#endif
#ifndef Uses_TStatusDef
#define Uses_TStatusDef
#endif
#ifndef Uses_TStatusItem
#define Uses_TStatusItem
#endif

#include <tvision/tv.h>

namespace ck::hotkeys
{

struct KeyBinding
{
    std::uint16_t command = 0;
    TKey key{};
    std::string display; // Human readable label such as "Ctrl-X".
};

struct Scheme
{
    std::string_view id;
    std::string_view displayName;
    std::string_view description;
    std::span<const KeyBinding> bindings;
};

struct CommandLabel
{
    std::uint16_t command = 0;
    std::string_view toolId;
    std::string label;
    std::string_view help{};
};

struct CommandHelp
{
    std::uint16_t command = 0;
    std::string_view text;
};

void registerSchemes(std::span<const Scheme> schemes);

void setActiveScheme(std::string_view id);

std::string_view activeScheme();

std::string defaultSchemeId();

std::string preferredScheme();

void setPreferredScheme(std::string_view id);

const KeyBinding *lookup(std::uint16_t command) noexcept;

TKey key(std::uint16_t command) noexcept;

std::string displayText(std::uint16_t command);

std::string statusLabel(std::uint16_t command, std::string_view action);

void configureMenuItem(TMenuItem &item);

void configureStatusItem(TStatusItem &item, std::string_view action);

void initializeFromEnvironment();

void registerDefaultSchemes();

void init();

void configureMenuTree(TMenuItem &root);

void applyCommandLineScheme(int &argc, char **argv);

void registerCommandLabels(std::span<const CommandLabel> labels, std::string_view locale = "en");

void registerCommandHelps(std::span<const CommandHelp> helps, std::string_view locale = "en");

bool customSchemeExists();

std::string customBaseScheme();

bool createCustomScheme(std::string_view templateId);

void clearCustomScheme();

std::string commandLabel(std::uint16_t command);

std::string commandTool(std::uint16_t command);

std::string commandHelp(std::uint16_t command);

void setLocale(std::string_view locale);

std::string_view activeLocale();

std::vector<std::uint16_t> commandsForTool(std::string_view toolId);

std::vector<std::uint16_t> allCommands();

std::vector<KeyBinding> schemeBindings(std::string_view schemeId);

std::vector<std::pair<std::string, std::string>> availableSchemes();

void setBinding(std::string_view schemeId, std::uint16_t command, TKey key, std::string display = {});

void clearBinding(std::string_view schemeId, std::uint16_t command);

std::string formatKey(TKey key);

void setCustomBindings(std::span<const KeyBinding> bindings, bool markDirty = true);

void saveCustomScheme();

} // namespace ck::hotkeys
