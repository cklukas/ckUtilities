#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/core/key.hpp>
#include <cvision/ui/command.hpp>

namespace ck::vision
{

// A single per-application override set. Keys are stable command identities;
// a null chord deliberately removes a command's default binding.
using KeymapOverrides = std::map<std::string, std::optional<ckv::KeyChord>>;

class KeymapPersistence
{
public:
    virtual ~KeymapPersistence() = default;

    virtual bool load(std::string_view application_id,
                      KeymapOverrides &global_overrides,
                      KeymapOverrides &application_overrides) = 0;
    virtual bool save(std::string_view application_id,
                      const KeymapOverrides &global_overrides,
                      const KeymapOverrides &application_overrides) = 0;
};

// The suite-owned JSON policy. The controller remains independent of paths,
// file I/O, and serialization so tests and alternate hosts can inject their
// own policy.
class DefaultKeymapPersistence final : public KeymapPersistence
{
public:
    bool load(std::string_view application_id,
              KeymapOverrides &global_overrides,
              KeymapOverrides &application_overrides) override;
    bool save(std::string_view application_id,
              const KeymapOverrides &global_overrides,
              const KeymapOverrides &application_overrides) override;
};

struct KeymapCommand
{
    std::string application_id;
    std::string key;
    std::string title;
    std::string category;
    std::optional<ckv::KeyChord> default_chord;
    std::optional<ckv::KeyChord> active_chord;
};

struct KeymapConflict
{
    std::string existing_command_key;
    ckv::KeyChord chord;
};

enum class KeymapUpdateStatus
{
    Applied,
    Conflict,
    UnknownCommand,
};

struct KeymapUpdate
{
    KeymapUpdateStatus status = KeymapUpdateStatus::UnknownCommand;
    std::optional<KeymapConflict> conflict;
};

// Applies suite keymap policy to one command registry. Commands beginning
// with "ckv." are shared framework bindings and reload in every native
// executable; all other command keys stay scoped to application_id.
class KeymapController
{
public:
    KeymapController(std::string application_id,
                     ckv::ui::CommandRegistry &commands,
                     KeymapPersistence &persistence);

    const std::string &application_id() const noexcept { return application_id_; }
    std::vector<KeymapCommand> commands() const;
    bool load();
    bool save();

    // Rejects an occupied chord unless replace_conflict is true. A replacement
    // is explicit because rebinding is destructive for the displaced command.
    KeymapUpdate update(std::string_view command_key,
                        std::optional<ckv::KeyChord> chord,
                        bool replace_conflict = false);
    bool reset(std::string_view command_key);
    KeymapPersistence &persistence() noexcept { return persistence_; }

private:
    static bool shared_command(std::string_view command_key) noexcept;
    KeymapOverrides &overrides_for(std::string_view command_key) noexcept;
    const KeymapOverrides &overrides_for(std::string_view command_key) const noexcept;
    void restore_defaults();
    void apply_overrides(const KeymapOverrides &overrides);
    void apply_binding(std::string_view command_key, const std::optional<ckv::KeyChord> &chord);

    std::string application_id_;
    ckv::ui::CommandRegistry &commands_;
    KeymapPersistence &persistence_;
    KeymapOverrides global_overrides_;
    KeymapOverrides application_overrides_;
};

// A suite-wide command catalog used only by the configuration application.
// It materializes command registries for executables that are not running, so
// their stable application bindings can be edited without importing a tool UI
// or invoking its services. Shared `ckv.*` bindings appear once through the
// active controller.
class SuiteKeymapCatalog
{
public:
    explicit SuiteKeymapCatalog(KeymapController &active_controller);
    ~SuiteKeymapCatalog();

    SuiteKeymapCatalog(const SuiteKeymapCatalog &) = delete;
    SuiteKeymapCatalog &operator=(const SuiteKeymapCatalog &) = delete;

    std::vector<KeymapCommand> commands() const;
    KeymapController *controller_for(std::string_view application_id) noexcept;
    bool load();

private:
    struct Entry;

    KeymapController *active_controller_;
    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace ck::vision
