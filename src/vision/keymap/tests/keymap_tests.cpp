#include "ck/vision/keymap.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

namespace
{

void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

class MemoryPersistence final : public ck::vision::KeymapPersistence
{
public:
    bool load(std::string_view application_id,
              ck::vision::KeymapOverrides &global_overrides,
              ck::vision::KeymapOverrides &application_overrides) override
    {
        global_overrides = global;
        application_overrides = applications[std::string(application_id)];
        return true;
    }

    bool save(std::string_view application_id,
              const ck::vision::KeymapOverrides &global_overrides,
              const ck::vision::KeymapOverrides &application_overrides) override
    {
        global = global_overrides;
        applications[std::string(application_id)] = application_overrides;
        return true;
    }

    ck::vision::KeymapOverrides global;
    std::map<std::string, ck::vision::KeymapOverrides> applications;
};

ckv::KeyChord chord(const char *text)
{
    const auto parsed = ckv::KeyChord::parse(text);
    require(parsed.has_value(), "Test chord must parse.");
    return *parsed;
}

} // namespace

int main()
{
    MemoryPersistence persistence;
    ckv::ui::CommandRegistry first_registry;
    const ckv::ui::CommandId first_open = first_registry.declare({.key = "ck.test.open", .title = "Open", .category = "Test", .chord = "Ctrl+O"});
    const ckv::ui::CommandId first_save = first_registry.declare({.key = "ck.test.save", .title = "Save", .category = "Test", .chord = "Ctrl+S"});
    ck::vision::KeymapController first("ck-test", first_registry, persistence);
    require(first.load(), "A missing in-memory keymap must load as defaults.");

    const auto rejected = first.update("ck.test.save", chord("Ctrl+O"));
    require(rejected.status == ck::vision::KeymapUpdateStatus::Conflict && rejected.conflict &&
                rejected.conflict->existing_command_key == "ck.test.open",
            "A duplicate chord must report its current command without changing bindings.");
    require(first_registry.command_for_key(chord("Ctrl+O")) == first_open &&
                first_registry.command_for_key(chord("Ctrl+S")) == first_save,
            "Rejected collisions must preserve both original bindings.");
    require(first.update("ck.test.save", chord("Ctrl+O"), true).status == ck::vision::KeymapUpdateStatus::Applied,
            "An explicit replacement must move the chord.");
    require(!first_registry.chord_for_command(first_open) && first_registry.command_for_key(chord("Ctrl+O")) == first_save,
            "Replacement must leave the displaced command unbound in this application.");
    require(first.save(), "Explicit keymap changes must save through the injected persistence policy.");

    ckv::ui::CommandRegistry reloaded_registry;
    const ckv::ui::CommandId reloaded_open = reloaded_registry.declare({.key = "ck.test.open", .title = "Open", .category = "Test", .chord = "Ctrl+O"});
    const ckv::ui::CommandId reloaded_save = reloaded_registry.declare({.key = "ck.test.save", .title = "Save", .category = "Test", .chord = "Ctrl+S"});
    ck::vision::KeymapController reloaded("ck-test", reloaded_registry, persistence);
    require(reloaded.load(), "Persisted application bindings must reload.");
    require(!reloaded_registry.chord_for_command(reloaded_open) &&
                reloaded_registry.command_for_key(chord("Ctrl+O")) == reloaded_save,
            "Reload must reproduce the explicit collision replacement.");

    require(first.update("ckv.app.quit", chord("Ctrl+Q")).status == ck::vision::KeymapUpdateStatus::Applied && first.save(),
            "A framework binding must persist as a global keymap override.");
    ckv::ui::CommandRegistry second_registry;
    ck::vision::KeymapController second("ck-other", second_registry, persistence);
    require(second.load(), "A second application must read the shared framework overrides.");
    require(second_registry.command_for_key(chord("Ctrl+Q")) == second_registry.standard().quit &&
                !second_registry.command_for_key(chord("Alt+X")),
            "A shared framework keymap update must reload in another native application.");

    const std::filesystem::path scheme_root = std::filesystem::temp_directory_path() /
                                              ("ckvision_keymap_scheme_" +
                                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::create_directories(scheme_root, error);
    require(!error, "The persistent-scheme test must create an isolated configuration root.");
    require(setenv("XDG_CONFIG_HOME", scheme_root.c_str(), 1) == 0,
            "The persistent-scheme test must direct keymap storage to its isolated root.");

    ck::vision::DefaultKeymapPersistence persisted_schemes;
    require(persisted_schemes.active_keymap_scheme() == "default",
            "A new installed keymap store must select built-in defaults.");
    ckv::ui::CommandRegistry scheme_registry;
    const ckv::ui::CommandId scheme_open = scheme_registry.declare(
        {.key = "ck.scheme.open", .title = "Open", .category = "Test", .chord = "Ctrl+O"});
    ck::vision::KeymapController scheme_controller("ck-scheme-test", scheme_registry, persisted_schemes);
    require(scheme_controller.load(), "The default shortcut scheme must load without a stored override document.");
    require(scheme_controller.update("ck.scheme.open", chord("Ctrl+Shift+O")).status == ck::vision::KeymapUpdateStatus::Applied &&
                scheme_controller.save() && persisted_schemes.active_keymap_scheme() == "personal",
            "The first binding edit must preserve defaults and switch to the personal scheme.");
    require(persisted_schemes.select_keymap_scheme("default") && scheme_controller.load() &&
                scheme_registry.command_for_key(chord("Ctrl+O")) == scheme_open &&
                !scheme_registry.command_for_key(chord("Ctrl+Shift+O")),
            "Selecting built-in defaults must leave the personal binding intact but inactive.");
    require(persisted_schemes.select_keymap_scheme("personal") && scheme_controller.load() &&
                scheme_registry.command_for_key(chord("Ctrl+Shift+O")) == scheme_open,
            "Re-selecting the personal scheme must restore its stable-key override.");
    const std::filesystem::path scheme_file = scheme_root / "ck-utilities" / "keymap.json";
    {
        std::ofstream legacy(scheme_file, std::ios::trunc);
        legacy << R"({"format_version":1,"global":{},"applications":{"ck-scheme-test":{"ck.scheme.open":"Ctrl+Shift+O"}}})";
    }
    require(persisted_schemes.active_keymap_scheme() == "personal" && scheme_controller.load() &&
                scheme_registry.command_for_key(chord("Ctrl+Shift+O")) == scheme_open && scheme_controller.save(),
            "A format-1 keymap must load as personal bindings and upgrade on its next write.");
    std::ifstream upgraded(scheme_file);
    const std::string upgraded_document{std::istreambuf_iterator<char>(upgraded), std::istreambuf_iterator<char>()};
    require(upgraded_document.find("\"format_version\": 2") != std::string::npos,
            "Saving a migrated keymap must write the versioned scheme document.");
    {
        std::ofstream corrupt(scheme_file, std::ios::trunc);
        corrupt << R"({"format_version":2,"active_scheme":17})";
    }
    require(!scheme_controller.load() && scheme_registry.command_for_key(chord("Ctrl+Shift+O")) == scheme_open,
            "A corrupt shortcut-scheme document must fail safely without changing bindings.");
    std::filesystem::remove_all(scheme_root, error);
    require(!error, "The persistent-scheme test must remove only its isolated configuration root.");
}
