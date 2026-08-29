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

} // namespace ck::vision
