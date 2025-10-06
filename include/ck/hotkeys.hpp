#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tvision/tkeys.h>
#include <tvision/menus.h>

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

void registerSchemes(std::span<const Scheme> schemes);

void setActiveScheme(std::string_view id);

std::string_view activeScheme();

const KeyBinding *lookup(std::uint16_t command) noexcept;

TKey key(std::uint16_t command) noexcept;

std::string displayText(std::uint16_t command);

std::string statusLabel(std::uint16_t command, std::string_view action);

void configureMenuItem(TMenuItem &item);

void configureStatusItem(TStatusItem &item, std::string_view action);

void initializeFromEnvironment();

void registerDefaultSchemes();

void configureMenuTree(TMenuItem &root);

void applyCommandLineScheme(int &argc, char **argv);

} // namespace ck::hotkeys
