#include "disk_usage_options.hpp"

namespace ck::du
{
namespace
{
const char *const kOptionSymlinkPolicy = "symlinkPolicy";
const char *const kOptionHardLinks = "countHardLinksMultiple";
const char *const kOptionIgnoreNodump = "ignoreNodump";
const char *const kOptionReportErrors = "reportErrors";
const char *const kOptionThreshold = "threshold";
const char *const kOptionStayOnFilesystem = "stayOnFilesystem";
const char *const kOptionIgnorePatterns = "ignorePatterns";
}

void registerDiskUsageOptions(config::OptionRegistry &registry)
{
    registry.registerOption({kOptionSymlinkPolicy, config::OptionKind::String,
                              config::OptionValue(std::string("never")), "Symlink Policy",
                              "Controls how symbolic links are followed."});
    registry.registerOption({kOptionHardLinks, config::OptionKind::Boolean, config::OptionValue(false),
                              "Count Hard Links Multiple Times",
                              "When enabled, files with multiple hard links are counted for each occurrence."});
    registry.registerOption({kOptionIgnoreNodump, config::OptionKind::Boolean, config::OptionValue(false),
                              "Ignore nodump Flag",
                              "Skip files and directories marked with the nodump flag."});
    registry.registerOption({kOptionReportErrors, config::OptionKind::Boolean, config::OptionValue(true),
                              "Report Read Errors",
                              "Display warnings for entries that cannot be read."});
    registry.registerOption({kOptionThreshold, config::OptionKind::Integer,
                              config::OptionValue(static_cast<std::int64_t>(0)), "Size Threshold",
                              "Show only entries that meet the threshold."});
    registry.registerOption({kOptionStayOnFilesystem, config::OptionKind::Boolean, config::OptionValue(false),
                              "Stay on One File System",
                              "Do not descend into directories on other file systems."});
    registry.registerOption({kOptionIgnorePatterns, config::OptionKind::StringList,
                              config::OptionValue(std::vector<std::string>{}), "Ignore Patterns",
                              "Filename patterns to exclude."});
}

BuildDirectoryTreeOptions buildDirectoryTreeOptionsFromRegistry(const config::OptionRegistry &registry)
{
    BuildDirectoryTreeOptions options;
    const std::string symlinkPolicy = registry.getString(kOptionSymlinkPolicy, "never");
    if (symlinkPolicy == "always")
        options.symlinkPolicy = BuildDirectoryTreeOptions::SymlinkPolicy::Always;
    else if (symlinkPolicy == "command-line")
    {
        options.symlinkPolicy = BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly;
        options.followCommandLineSymlinks = true;
    }

    options.countHardLinksMultipleTimes = registry.getBool(kOptionHardLinks);
    options.ignoreNodumpFlag = registry.getBool(kOptionIgnoreNodump);
    options.reportErrors = registry.getBool(kOptionReportErrors, true);
    options.threshold = registry.getInteger(kOptionThreshold);
    options.stayOnFilesystem = registry.getBool(kOptionStayOnFilesystem);
    options.ignoreMasks = registry.getStringList(kOptionIgnorePatterns);
    return options;
}

} // namespace ck::du
