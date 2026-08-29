#include "config_persistence.hpp"

namespace ck::vision
{

bool DefaultConfigPersistence::load(ck::config::OptionRegistry &registry)
{
    return registry.loadDefaults();
}

bool DefaultConfigPersistence::save(const ck::config::OptionRegistry &registry)
{
    return registry.saveDefaults();
}

bool DefaultConfigPersistence::import_from(ck::config::OptionRegistry &registry, const std::string &path)
{
    return registry.loadFromFile(path);
}

bool DefaultConfigPersistence::export_to(const ck::config::OptionRegistry &registry, const std::string &path)
{
    return registry.saveToFile(path);
}

} // namespace ck::vision
