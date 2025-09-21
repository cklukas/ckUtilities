#include "ck/options.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace ck::config
{
namespace
{
bool parseBool(const std::string &value, bool fallback)
{
    std::string lower;
    lower.reserve(value.size());
    for (char ch : value)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        return false;
    return fallback;
}

std::int64_t parseInteger(const std::string &value, std::int64_t fallback)
{
    try
    {
        size_t idx = 0;
        std::int64_t parsed = std::stoll(value, &idx, 0);
        if (idx == value.size())
            return parsed;
    }
    catch (...)
    {
    }
    return fallback;
}

nlohmann::json toJson(const OptionValue &value)
{
    switch (value.type())
    {
    case OptionValueType::Boolean:
        return value.toBool();
    case OptionValueType::Integer:
        return value.toInteger();
    case OptionValueType::String:
        return value.toString();
    case OptionValueType::StringList:
        return value.toStringList();
    case OptionValueType::None:
    default:
        return nlohmann::json();
    }
}

OptionValue fromJson(const OptionDefinition &definition, const nlohmann::json &jsonValue)
{
    switch (definition.kind)
    {
    case OptionKind::Boolean:
        if (jsonValue.is_boolean())
            return OptionValue(jsonValue.get<bool>());
        if (jsonValue.is_number_integer())
            return OptionValue(jsonValue.get<std::int64_t>() != 0);
        if (jsonValue.is_string())
            return OptionValue(parseBool(jsonValue.get<std::string>(), definition.defaultValue.toBool()));
        break;
    case OptionKind::Integer:
        if (jsonValue.is_number_integer())
            return OptionValue(jsonValue.get<std::int64_t>());
        if (jsonValue.is_boolean())
            return OptionValue(jsonValue.get<bool>() ? std::int64_t{1} : std::int64_t{0});
        if (jsonValue.is_string())
            return OptionValue(parseInteger(jsonValue.get<std::string>(), definition.defaultValue.toInteger()));
        break;
    case OptionKind::String:
        if (jsonValue.is_string())
            return OptionValue(jsonValue.get<std::string>());
        if (jsonValue.is_boolean())
            return OptionValue(jsonValue.get<bool>() ? std::string("true") : std::string("false"));
        if (jsonValue.is_number_integer())
            return OptionValue(std::to_string(jsonValue.get<std::int64_t>()));
        break;
    case OptionKind::StringList:
        if (jsonValue.is_array())
        {
            std::vector<std::string> result;
            for (const auto &item : jsonValue)
            {
                if (item.is_string())
                    result.push_back(item.get<std::string>());
            }
            return OptionValue(result);
        }
        if (jsonValue.is_string())
            return OptionValue(std::vector<std::string>{jsonValue.get<std::string>()});
        break;
    }
    return definition.defaultValue;
}

std::filesystem::path detectConfigRoot()
{
#ifdef _WIN32
    if (const char *appData = std::getenv("APPDATA"))
    {
        std::filesystem::path path(appData);
        if (!path.empty())
            return path / "ck-utilities";
    }
#endif
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
    {
        std::filesystem::path path(xdg);
        if (!path.empty())
            return path / "ck-utilities";
    }
    if (const char *home = std::getenv("HOME"))
    {
        std::filesystem::path path(home);
        if (!path.empty())
            return path / ".config" / "ck-utilities";
    }
#ifdef _WIN32
    return std::filesystem::path("ck-utilities");
#else
    return std::filesystem::path(".config") / "ck-utilities";
#endif
}

} // namespace

OptionValue::OptionValue(bool value)
    : value(value)
{
}

OptionValue::OptionValue(std::int64_t value)
    : value(value)
{
}

OptionValue::OptionValue(std::string value)
    : value(std::move(value))
{
}

OptionValue::OptionValue(std::vector<std::string> value)
    : value(std::move(value))
{
}

OptionValueType OptionValue::type() const noexcept
{
    return storageType(value);
}

bool OptionValue::isNull() const noexcept
{
    return std::holds_alternative<std::monostate>(value);
}

bool OptionValue::toBool(bool fallback) const noexcept
{
    if (auto *ptr = std::get_if<bool>(&value))
        return *ptr;
    if (auto *iptr = std::get_if<std::int64_t>(&value))
        return *iptr != 0;
    if (auto *sptr = std::get_if<std::string>(&value))
        return parseBool(*sptr, fallback);
    return fallback;
}

std::int64_t OptionValue::toInteger(std::int64_t fallback) const noexcept
{
    if (auto *iptr = std::get_if<std::int64_t>(&value))
        return *iptr;
    if (auto *bptr = std::get_if<bool>(&value))
        return *bptr ? 1 : 0;
    if (auto *sptr = std::get_if<std::string>(&value))
        return parseInteger(*sptr, fallback);
    return fallback;
}

std::string OptionValue::toString(const std::string &fallback) const
{
    if (auto *sptr = std::get_if<std::string>(&value))
        return *sptr;
    if (auto *bptr = std::get_if<bool>(&value))
        return *bptr ? "true" : "false";
    if (auto *iptr = std::get_if<std::int64_t>(&value))
        return std::to_string(*iptr);
    return fallback;
}

std::vector<std::string> OptionValue::toStringList() const
{
    if (auto *lptr = std::get_if<std::vector<std::string>>(&value))
        return *lptr;
    if (auto *sptr = std::get_if<std::string>(&value))
        return {*sptr};
    return {};
}

bool OptionValue::operator==(const OptionValue &other) const noexcept
{
    return value == other.value;
}

OptionValueType OptionValue::storageType(const OptionValue::Storage &storage) noexcept
{
    switch (storage.index())
    {
    case 1:
        return OptionValueType::Boolean;
    case 2:
        return OptionValueType::Integer;
    case 3:
        return OptionValueType::String;
    case 4:
        return OptionValueType::StringList;
    default:
        return OptionValueType::None;
    }
}

OptionRegistry::OptionRegistry(std::string appId)
    : id(std::move(appId))
{
}

void OptionRegistry::registerOption(const OptionDefinition &definition)
{
    OptionDefinition normalized = definition;
    definitions[definition.key] = normalized;
    auto it = overrides.find(definition.key);
    if (it != overrides.end())
        overrides[definition.key] = normalizeValue(definition, it->second);
}

bool OptionRegistry::hasOption(const std::string &key) const noexcept
{
    return definitions.find(key) != definitions.end();
}

void OptionRegistry::set(const std::string &key, const OptionValue &value)
{
    const OptionDefinition *definition = findDefinition(key);
    if (!definition)
        return;
    overrides[key] = normalizeValue(*definition, value);
}

void OptionRegistry::reset(const std::string &key)
{
    overrides.erase(key);
}

OptionValue OptionRegistry::get(const std::string &key) const
{
    auto overrideIt = overrides.find(key);
    if (overrideIt != overrides.end())
        return overrideIt->second;
    const OptionDefinition *definition = findDefinition(key);
    if (definition)
        return definition->defaultValue;
    return OptionValue();
}

bool OptionRegistry::getBool(const std::string &key, bool fallback) const
{
    return get(key).toBool(fallback);
}

std::int64_t OptionRegistry::getInteger(const std::string &key, std::int64_t fallback) const
{
    return get(key).toInteger(fallback);
}

std::string OptionRegistry::getString(const std::string &key, const std::string &fallback) const
{
    return get(key).toString(fallback);
}

std::vector<std::string> OptionRegistry::getStringList(const std::string &key) const
{
    return get(key).toStringList();
}

void OptionRegistry::clearValues() noexcept
{
    overrides.clear();
}

void OptionRegistry::resetToDefaults()
{
    overrides.clear();
}

bool OptionRegistry::loadFromFile(const std::filesystem::path &filePath)
{
    std::ifstream in(filePath);
    if (!in)
        return false;

    nlohmann::json data;
    try
    {
        in >> data;
    }
    catch (...)
    {
        return false;
    }

    if (!data.is_object())
        return false;

    for (auto it = data.begin(); it != data.end(); ++it)
    {
        const std::string key = it.key();
        const OptionDefinition *definition = findDefinition(key);
        if (!definition)
            continue;
        OptionValue parsed = fromJson(*definition, it.value());
        overrides[key] = normalizeValue(*definition, parsed);
    }

    return true;
}

bool OptionRegistry::saveToFile(const std::filesystem::path &filePath) const
{
    nlohmann::json data = nlohmann::json::object();
    for (const auto &[key, definition] : definitions)
    {
        OptionValue current = get(key);
        if (current.type() == OptionValueType::None)
        {
            data[key] = nullptr;
            continue;
        }
        data[key] = toJson(current);
    }

    std::error_code ec;
    std::filesystem::create_directories(filePath.parent_path(), ec);

    std::ofstream out(filePath);
    if (!out)
        return false;
    out << data.dump(2) << std::endl;
    return static_cast<bool>(out);
}

bool OptionRegistry::loadDefaults()
{
    std::filesystem::path path = defaultOptionsPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return false;
    return loadFromFile(path);
}

bool OptionRegistry::saveDefaults() const
{
    return saveToFile(defaultOptionsPath());
}

bool OptionRegistry::clearDefaults() const
{
    std::filesystem::path path = defaultOptionsPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return true;
    return std::filesystem::remove(path, ec);
}

std::filesystem::path OptionRegistry::defaultOptionsPath() const
{
    return configRoot() / id / "defaults.json";
}

std::unordered_map<std::string, OptionValue> OptionRegistry::values() const
{
    std::unordered_map<std::string, OptionValue> result;
    for (const auto &[key, definition] : definitions)
    {
        (void)definition;
        result[key] = get(key);
    }
    return result;
}

std::vector<OptionDefinition> OptionRegistry::listRegisteredOptions() const
{
    std::vector<OptionDefinition> result;
    result.reserve(definitions.size());
    for (const auto &[key, definition] : definitions)
        result.push_back(definition);
    std::sort(result.begin(), result.end(), [](const OptionDefinition &a, const OptionDefinition &b) {
        return a.displayName < b.displayName;
    });
    return result;
}

const OptionDefinition *OptionRegistry::definition(const std::string &key) const
{
    return findDefinition(key);
}

std::filesystem::path OptionRegistry::configRoot()
{
    static std::filesystem::path root = detectConfigRoot();
    return root;
}

std::vector<std::string> OptionRegistry::availableProfiles()
{
    std::vector<std::string> profiles;
    std::filesystem::path root = configRoot();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return profiles;

    for (const auto &entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!entry.is_directory(ec))
            continue;
        std::filesystem::path defaults = entry.path() / "defaults.json";
        if (std::filesystem::exists(defaults, ec))
            profiles.push_back(entry.path().filename().string());
    }

    std::sort(profiles.begin(), profiles.end());
    profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
    return profiles;
}

const OptionDefinition *OptionRegistry::findDefinition(const std::string &key) const
{
    auto it = definitions.find(key);
    if (it == definitions.end())
        return nullptr;
    return &it->second;
}

OptionValue OptionRegistry::normalizeValue(const OptionDefinition &definition, const OptionValue &value) const
{
    switch (definition.kind)
    {
    case OptionKind::Boolean:
        return OptionValue(value.toBool(definition.defaultValue.toBool()));
    case OptionKind::Integer:
        return OptionValue(value.toInteger(definition.defaultValue.toInteger()));
    case OptionKind::String:
        return OptionValue(value.toString(definition.defaultValue.toString()));
    case OptionKind::StringList:
        return OptionValue(value.toStringList());
    }
    return value;
}

} // namespace ck::config

