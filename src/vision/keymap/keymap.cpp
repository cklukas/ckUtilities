#include "ck/vision/keymap.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "ck/options.hpp"

namespace ck::vision
{
namespace
{

constexpr int kKeymapFormatVersion = 1;

std::filesystem::path keymap_path()
{
    return ck::config::OptionRegistry::configRoot() / "keymap.json";
}

bool read_overrides(const nlohmann::json &source, KeymapOverrides &overrides)
{
    overrides.clear();
    if (!source.is_object())
        return false;
    for (auto entry = source.begin(); entry != source.end(); ++entry)
    {
        if (entry.value().is_null())
        {
            overrides.emplace(entry.key(), std::nullopt);
            continue;
        }
        if (!entry.value().is_string())
            return false;
        const auto chord = ckv::KeyChord::parse(entry.value().get<std::string>());
        if (!chord)
            return false;
        overrides.emplace(entry.key(), *chord);
    }
    return true;
}

nlohmann::json write_overrides(const KeymapOverrides &overrides)
{
    nlohmann::json result = nlohmann::json::object();
    for (const auto &[key, chord] : overrides)
        result[key] = chord ? nlohmann::json(ckv::format(*chord)) : nlohmann::json(nullptr);
    return result;
}

bool read_document(nlohmann::json &document)
{
    const std::filesystem::path path = keymap_path();
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return !error;
    if (error)
        return false;
    std::ifstream input(path);
    if (!input)
        return false;
    try
    {
        input >> document;
        return document.is_object() && document.value("format_version", 0) == kKeymapFormatVersion;
    }
    catch (const nlohmann::json::exception &)
    {
        return false;
    }
}

} // namespace

bool DefaultKeymapPersistence::load(std::string_view application_id,
                                    KeymapOverrides &global_overrides,
                                    KeymapOverrides &application_overrides)
{
    global_overrides.clear();
    application_overrides.clear();
    nlohmann::json document = nlohmann::json::object();
    if (!read_document(document))
        return false;
    if (document.empty())
        return true;

    const nlohmann::json global = document.value("global", nlohmann::json::object());
    const nlohmann::json applications = document.value("applications", nlohmann::json::object());
    if (!read_overrides(global, global_overrides) || !applications.is_object())
        return false;
    const auto application = applications.find(std::string(application_id));
    return application == applications.end() || read_overrides(*application, application_overrides);
}

bool DefaultKeymapPersistence::save(std::string_view application_id,
                                    const KeymapOverrides &global_overrides,
                                    const KeymapOverrides &application_overrides)
{
    nlohmann::json document = nlohmann::json::object();
    if (!read_document(document))
        return false;
    if (document.empty())
        document["format_version"] = kKeymapFormatVersion;
    document["global"] = write_overrides(global_overrides);
    if (!document.contains("applications") || !document["applications"].is_object())
        document["applications"] = nlohmann::json::object();
    document["applications"][std::string(application_id)] = write_overrides(application_overrides);

    const std::filesystem::path path = keymap_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
            return false;
        output << document.dump(2) << '\n';
        output.flush();
        if (!output)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

KeymapController::KeymapController(std::string application_id,
                                   ckv::ui::CommandRegistry &commands,
                                   KeymapPersistence &persistence)
    : application_id_(std::move(application_id)), commands_(commands), persistence_(persistence)
{
}

std::vector<KeymapCommand> KeymapController::commands() const
{
    std::vector<KeymapCommand> result;
    for (const ckv::ui::CommandInfo &command : commands_.all())
        result.push_back({application_id_, command.key, command.title, command.category, command.default_chord,
                          commands_.chord_for_command(command.id)});
    return result;
}

bool KeymapController::load()
{
    KeymapOverrides global;
    KeymapOverrides application;
    if (!persistence_.load(application_id_, global, application))
        return false;
    global_overrides_ = std::move(global);
    application_overrides_ = std::move(application);
    restore_defaults();
    apply_overrides(global_overrides_);
    apply_overrides(application_overrides_);
    return true;
}

bool KeymapController::save()
{
    return persistence_.save(application_id_, global_overrides_, application_overrides_);
}

KeymapUpdate KeymapController::update(std::string_view command_key,
                                      std::optional<ckv::KeyChord> chord,
                                      bool replace_conflict)
{
    const auto target = commands_.id_for(command_key);
    if (!target)
        return {KeymapUpdateStatus::UnknownCommand, std::nullopt};

    if (chord)
    {
        if (const auto occupied = commands_.command_for_key(*chord); occupied && *occupied != *target)
        {
            const std::string occupied_key(commands_.key_for(*occupied));
            if (!replace_conflict)
                return {KeymapUpdateStatus::Conflict, KeymapConflict{occupied_key, *chord}};
            commands_.unbind_key(*chord);
            if (shared_command(command_key) || !shared_command(occupied_key))
                overrides_for(occupied_key)[occupied_key] = std::nullopt;
        }
    }

    if (const auto previous = commands_.chord_for_command(*target))
        commands_.unbind_key(*previous);
    if (chord)
        commands_.bind_key(*chord, *target);
    overrides_for(command_key)[std::string(command_key)] = chord;
    return {KeymapUpdateStatus::Applied, std::nullopt};
}

bool KeymapController::reset(std::string_view command_key)
{
    const auto target = commands_.id_for(command_key);
    if (!target)
        return false;
    overrides_for(command_key).erase(std::string(command_key));
    restore_defaults();
    apply_overrides(global_overrides_);
    apply_overrides(application_overrides_);
    return true;
}

bool KeymapController::shared_command(std::string_view command_key) noexcept
{
    return command_key.starts_with("ckv.");
}

KeymapOverrides &KeymapController::overrides_for(std::string_view command_key) noexcept
{
    return shared_command(command_key) ? global_overrides_ : application_overrides_;
}

const KeymapOverrides &KeymapController::overrides_for(std::string_view command_key) const noexcept
{
    return shared_command(command_key) ? global_overrides_ : application_overrides_;
}

void KeymapController::restore_defaults()
{
    for (const ckv::ui::CommandInfo &command : commands_.all())
    {
        if (const auto active = commands_.chord_for_command(command.id))
            commands_.unbind_key(*active);
        if (command.default_chord)
            commands_.bind_key(*command.default_chord, command.id);
    }
}

void KeymapController::apply_overrides(const KeymapOverrides &overrides)
{
    for (const auto &[command_key, chord] : overrides)
        apply_binding(command_key, chord);
}

void KeymapController::apply_binding(std::string_view command_key, const std::optional<ckv::KeyChord> &chord)
{
    const auto target = commands_.id_for(command_key);
    if (!target)
        return;
    if (const auto previous = commands_.chord_for_command(*target))
        commands_.unbind_key(*previous);
    if (!chord)
        return;
    if (const auto occupied = commands_.command_for_key(*chord); occupied && *occupied != *target)
        commands_.unbind_key(*chord);
    commands_.bind_key(*chord, *target);
}

} // namespace ck::vision
