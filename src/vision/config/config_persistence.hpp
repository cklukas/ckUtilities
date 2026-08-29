#pragma once

#include <string>

#include "ck/options.hpp"

namespace ck::vision
{

// Composition-root policy for option persistence.  The configuration view
// receives this capability instead of selecting config paths or filesystem
// APIs itself.
class ConfigPersistence
{
public:
    virtual ~ConfigPersistence() = default;

    virtual bool load(ck::config::OptionRegistry &registry) = 0;
    virtual bool save(const ck::config::OptionRegistry &registry) = 0;
    virtual bool import_from(ck::config::OptionRegistry &registry, const std::string &path) = 0;
    virtual bool export_to(const ck::config::OptionRegistry &registry, const std::string &path) = 0;
};

// Retains the suite's JSON defaults format while keeping its location outside
// the presentation layer.
class DefaultConfigPersistence final : public ConfigPersistence
{
public:
    bool load(ck::config::OptionRegistry &registry) override;
    bool save(const ck::config::OptionRegistry &registry) override;
    bool import_from(ck::config::OptionRegistry &registry, const std::string &path) override;
    bool export_to(const ck::config::OptionRegistry &registry, const std::string &path) override;
};

} // namespace ck::vision
