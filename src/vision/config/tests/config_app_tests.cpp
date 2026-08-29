#include "config_app.hpp"

#include <cstdlib>
#include <iostream>
#include <map>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

namespace
{
void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

class MemoryPersistence final : public ck::vision::ConfigPersistence
{
public:
    bool load(ck::config::OptionRegistry &registry) override
    {
        ++load_count;
        registry.set("enabled", ck::config::OptionValue(true));
        return true;
    }

    bool save(const ck::config::OptionRegistry &registry) override
    {
        ++save_count;
        saved_enabled = registry.getBool("enabled");
        return true;
    }

    bool import_from(ck::config::OptionRegistry &registry, const std::string &path) override
    {
        ++import_count;
        imported_path = path;
        registry.set("enabled", ck::config::OptionValue(true));
        return !fail_import_after_mutation;
    }

    bool export_to(const ck::config::OptionRegistry &registry, const std::string &path) override
    {
        ++export_count;
        exported_path = path;
        exported_enabled = registry.getBool("enabled");
        return true;
    }

    int load_count = 0;
    int save_count = 0;
    bool saved_enabled = false;
    int import_count = 0;
    int export_count = 0;
    std::string imported_path;
    std::string exported_path;
    bool exported_enabled = false;
    bool fail_import_after_mutation = false;
};

class MemoryKeymapPersistence final : public ck::vision::KeymapPersistence
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
}

int main()
{
    ck::config::OptionRegistry registry("test");
    registry.registerOption({"enabled", ck::config::OptionKind::Boolean, ck::config::OptionValue(false), "Enabled", "Enables the test feature."});
    registry.registerOption({"limit", ck::config::OptionKind::Integer, ck::config::OptionValue(std::int64_t{7}), "Limit", "Sets the test limit."});
    registry.set("enabled", ck::config::OptionValue(true));

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    MemoryPersistence persistence;
    ck::vision::ConfigApp config(application, registry, persistence);
    require(config.option_count() == 2 && config.table() != nullptr,
            "The native config app must present injected registry definitions in a table.");
    require(config.select_option("enabled"), "The native config app must select options by stable string key.");
    require(application.execute_command(config.reset_command()), "Reset must dispatch through the command registry.");
    require(!registry.getBool("enabled"), "Reset must restore the selected option's registered default.");
    registry.set("enabled", ck::config::OptionValue(true));
    require(application.execute_command(config.save_command()), "Save must dispatch through the command registry.");
    require(persistence.save_count == 1 && persistence.saved_enabled,
            "Save must delegate the current registry state to the injected persistence policy.");
    registry.set("enabled", ck::config::OptionValue(false));
    require(application.execute_command(config.reload_command()), "Reload must dispatch through the command registry.");
    require(persistence.load_count == 1 && registry.getBool("enabled"),
            "Reload must delegate registry updates to the injected persistence policy.");
    require(config.export_configuration("/exports/test.json"),
            "Export must delegate the registry to the injected persistence policy.");
    require(persistence.export_count == 1 && persistence.exported_path == "/exports/test.json" && persistence.exported_enabled,
            "Export must preserve the requested destination and current option values.");
    registry.set("enabled", ck::config::OptionValue(false));
    require(config.import_configuration("/imports/test.json"),
            "Import must delegate registry updates to the injected persistence policy.");
    require(persistence.import_count == 1 && persistence.imported_path == "/imports/test.json" && registry.getBool("enabled"),
            "Import must refresh the registry through the injected persistence policy.");
    registry.set("enabled", ck::config::OptionValue(false));
    persistence.fail_import_after_mutation = true;
    require(!config.import_configuration("/imports/broken.json") && !registry.getBool("enabled"),
            "A failed import must roll back mutations made by its persistence policy.");
    require(application.execute_command(config.import_command()) && application.execute_command(config.export_command()),
            "Import and export must be registry commands.");
    require(application.execute_command(config.edit_command()), "Edit must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native config view must render headlessly.");

    ckv::ManualClock keymap_clock;
    ckv::term::HeadlessTerminal keymap_terminal(ckv::Size{100, 30});
    ckv::ui::Application keymap_application(keymap_terminal, keymap_clock);
    ck::config::OptionRegistry keymap_registry("keymap-test");
    keymap_registry.registerOption({"enabled", ck::config::OptionKind::Boolean, ck::config::OptionValue(false), "Enabled", "Test option."});
    MemoryPersistence keymap_config_persistence;
    MemoryKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-config", keymap_application.commands(), keymap_persistence);
    ck::vision::ConfigApp keymap_config(keymap_application, keymap_registry, keymap_config_persistence, &keymap);
    require(keymap.load(), "The native config app's injected keymap must load after its commands are declared.");
    require(keymap_config.keymap_command_count() > keymap.commands().size(),
            "The native config app must expose a catalog of stable command identities beyond its own executable.");
    require(keymap_application.execute_command(keymap_config.keymap_command()),
            "Keyboard shortcut configuration must be a command-registry action.");
    keymap_application.step(0);
    require(keymap_application.current_frame().size() == ckv::Size{100, 30},
            "The native keyboard shortcut table must render headlessly.");
    require(keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Tab, ckv::Modifier::None, {}}}) &&
                keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}}),
            "The shortcut table must lead by keyboard to the native shortcut editor.");
    require(keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}}) &&
                keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "q"}}),
            "Shortcut capture must consume the requested command chord rather than dispatch it.");
    require(keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Tab, ckv::Modifier::None, {}}}) &&
                keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}}),
            "The captured shortcut must be applicable through keyboard-only interaction.");
    const auto saved_quit = keymap_persistence.global.find("ckv.app.quit");
    require(saved_quit != keymap_persistence.global.end() && saved_quit->second &&
                *saved_quit->second == ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl, "q"},
            "Applying a shared shortcut must persist its normalized typed chord.");
    keymap_application.step(0);
    require(keymap_config.select_keymap_command("ck-edit", "ck.edit.save"),
            "The suite shortcut catalog must select an application-specific command from another native executable.");
    require(keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Tab, ckv::Modifier::None, {}}}) &&
                keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}}),
            "A selected suite command must open through the same keyboard-only shortcut editor.");
    require(keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}}) &&
                keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Char,
                                                                          ckv::Modifier::Ctrl | ckv::Modifier::Shift,
                                                                          "s"}}),
            "Cross-application capture must consume the requested typed chord.");
    require(keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Tab, ckv::Modifier::None, {}}}) &&
                keymap_application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, {}}}),
            "A cross-application shortcut must apply through the ordinary capture workflow.");
    const auto saved_editor = keymap_persistence.applications["ck-edit"].find("ck.edit.save");
    require(saved_editor != keymap_persistence.applications["ck-edit"].end() && saved_editor->second &&
                *saved_editor->second == ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl | ckv::Modifier::Shift, "s"},
            "A suite-owned application binding must persist under its target executable rather than the config host.");
    ckv::ui::CommandRegistry editor_registry;
    const ckv::ui::CommandId editor_save = ck::vision::declare_suite_command(
        editor_registry, "ck-edit", "ck.edit.save", [] {});
    ck::vision::KeymapController reloaded_editor("ck-edit", editor_registry, keymap_persistence);
    require(reloaded_editor.load() &&
                editor_registry.command_for_key(ckv::KeyChord{ckv::Key::Char, ckv::Modifier::Ctrl | ckv::Modifier::Shift, "s"}) == editor_save,
            "A configured application binding must reload when its target executable creates its own native registry.");
}
