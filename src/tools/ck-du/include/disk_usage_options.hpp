#pragma once

#include "ck/options.hpp"
#include "disk_usage_core.hpp"

namespace ck::du
{

void registerDiskUsageOptions(config::OptionRegistry &registry);

// Converts the persisted ck-du profile into the scanning options consumed by
// the native core. Call this after registering options and loading the
// profile, so configuration changes take effect on the next application run.
BuildDirectoryTreeOptions buildDirectoryTreeOptionsFromRegistry(const config::OptionRegistry &registry);

} // namespace ck::du
