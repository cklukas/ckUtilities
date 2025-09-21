#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ck::config
{

enum class OptionKind
{
    Boolean,
    Integer,
    String,
    StringList
};

enum class OptionValueType
{
    None,
    Boolean,
    Integer,
    String,
    StringList
};

class OptionValue
{
public:
    OptionValue() = default;
    OptionValue(bool value);
    OptionValue(std::int64_t value);
    OptionValue(std::string value);
    OptionValue(std::vector<std::string> value);

    OptionValueType type() const noexcept;

    bool isNull() const noexcept;
    bool toBool(bool fallback = false) const noexcept;
    std::int64_t toInteger(std::int64_t fallback = 0) const noexcept;
    std::string toString(const std::string &fallback = std::string()) const;
    std::vector<std::string> toStringList() const;

    bool operator==(const OptionValue &other) const noexcept;
    bool operator!=(const OptionValue &other) const noexcept { return !(*this == other); }

private:
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::string, std::vector<std::string>>;
    friend class OptionRegistry;
    static OptionValueType storageType(const Storage &storage) noexcept;
    Storage value;
};

struct OptionDefinition
{
    std::string key;
    OptionKind kind = OptionKind::String;
    OptionValue defaultValue;
    std::string displayName;
    std::string description;
};

class OptionRegistry
{
public:
    explicit OptionRegistry(std::string appId);

    const std::string &appId() const noexcept { return id; }

    void registerOption(const OptionDefinition &definition);
    bool hasOption(const std::string &key) const noexcept;

    void set(const std::string &key, const OptionValue &value);
    void reset(const std::string &key);

    OptionValue get(const std::string &key) const;
    bool getBool(const std::string &key, bool fallback = false) const;
    std::int64_t getInteger(const std::string &key, std::int64_t fallback = 0) const;
    std::string getString(const std::string &key, const std::string &fallback = std::string()) const;
    std::vector<std::string> getStringList(const std::string &key) const;

    void clearValues() noexcept;
    void resetToDefaults();

    bool loadFromFile(const std::filesystem::path &filePath);
    bool saveToFile(const std::filesystem::path &filePath) const;

    bool loadDefaults();
    bool saveDefaults() const;
    bool clearDefaults() const;
    std::filesystem::path defaultOptionsPath() const;

    std::unordered_map<std::string, OptionValue> values() const;
    std::vector<OptionDefinition> listRegisteredOptions() const;
    const OptionDefinition *definition(const std::string &key) const;

    static std::filesystem::path configRoot();
    static std::vector<std::string> availableProfiles();

private:
    const OptionDefinition *findDefinition(const std::string &key) const;
    OptionValue normalizeValue(const OptionDefinition &definition, const OptionValue &value) const;

    std::string id;
    std::unordered_map<std::string, OptionDefinition> definitions;
    std::unordered_map<std::string, OptionValue> overrides;
};

} // namespace ck::config

