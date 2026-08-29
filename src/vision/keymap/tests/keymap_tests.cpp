#include "ck/vision/keymap.hpp"

#include <cstdlib>
#include <iostream>
#include <map>

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
}
