#include "ck/find/search_backend.hpp"

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/options.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <regex>
#include <initializer_list>

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace ck::find
{
namespace
{

template <std::size_t N>
std::string arrayToString(const std::array<char, N> &buffer)
{
    return bufferToString(buffer);
}

template <std::size_t N>
void assignToArray(std::array<char, N> &buffer, const std::string &value)
{
    copyToArray(buffer, value.c_str());
}

std::string trimCopy(std::string_view value)
{
    std::size_t start = 0;
    std::size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return std::string(value.substr(start, end - start));
}

std::string slugify(const std::string &name)
{
    std::string slug;
    slug.reserve(name.size());
    bool lastWasDash = false;
    for (unsigned char ch : name)
    {
        if (std::isalnum(ch))
        {
            slug.push_back(static_cast<char>(std::tolower(ch)));
            lastWasDash = false;
        }
        else if (ch == '-' || ch == '_' || std::isspace(ch))
        {
            if (!lastWasDash && !slug.empty())
            {
                slug.push_back('-');
                lastWasDash = true;
            }
        }
        else if (ch == '.')
        {
            if (!lastWasDash && !slug.empty())
            {
                slug.push_back('-');
                lastWasDash = true;
            }
        }
    }
    while (!slug.empty() && slug.front() == '-')
        slug.erase(slug.begin());
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    if (slug.empty())
        slug = "spec";
    if (slug.size() > 64)
        slug.resize(64);
    return slug;
}

std::filesystem::path storageDirectory()
{
    std::filesystem::path base = ck::config::OptionRegistry::configRoot();
    base /= "ck-find";
    base /= "specs";
    std::error_code ec;
    if (!std::filesystem::exists(base, ec))
        std::filesystem::create_directories(base, ec);
    return base;
}

std::vector<std::string> splitList(const std::string &value, char separator = ',')
{
    std::vector<std::string> items;
    std::string current;
    std::istringstream stream(value);
    while (std::getline(stream, current, separator))
    {
        current = trimCopy(current);
        if (!current.empty())
            items.push_back(current);
    }
    return items;
}

nlohmann::json toJson(const TextSearchOptions &options);
nlohmann::json toJson(const NamePathOptions &options);
nlohmann::json toJson(const TimeFilterOptions &options);
nlohmann::json toJson(const SizeFilterOptions &options);
nlohmann::json toJson(const TypeFilterOptions &options);
nlohmann::json toJson(const PermissionOwnershipOptions &options);
nlohmann::json toJson(const TraversalFilesystemOptions &options);
nlohmann::json toJson(const ActionOptions &options);

void fromJson(const nlohmann::json &j, TextSearchOptions &options);
void fromJson(const nlohmann::json &j, NamePathOptions &options);
void fromJson(const nlohmann::json &j, TimeFilterOptions &options);
void fromJson(const nlohmann::json &j, SizeFilterOptions &options);
void fromJson(const nlohmann::json &j, TypeFilterOptions &options);
void fromJson(const nlohmann::json &j, PermissionOwnershipOptions &options);
void fromJson(const nlohmann::json &j, TraversalFilesystemOptions &options);
void fromJson(const nlohmann::json &j, ActionOptions &options);

nlohmann::json toJson(const SearchSpecification &spec)
{
    nlohmann::json j;
    j["specName"] = arrayToString(spec.specName);
    j["startLocation"] = arrayToString(spec.startLocation);
    j["searchText"] = arrayToString(spec.searchText);
    j["includePatterns"] = arrayToString(spec.includePatterns);
    j["excludePatterns"] = arrayToString(spec.excludePatterns);
    j["includeSubdirectories"] = spec.includeSubdirectories;
    j["includeHidden"] = spec.includeHidden;
    j["followSymlinks"] = spec.followSymlinks;
    j["stayOnSameFilesystem"] = spec.stayOnSameFilesystem;
    j["enableTextSearch"] = spec.enableTextSearch;
    j["enableNamePathTests"] = spec.enableNamePathTests;
    j["enableTimeFilters"] = spec.enableTimeFilters;
    j["enableSizeFilters"] = spec.enableSizeFilters;
    j["enableTypeFilters"] = spec.enableTypeFilters;
    j["enablePermissionOwnership"] = spec.enablePermissionOwnership;
    j["enableTraversalFilters"] = spec.enableTraversalFilters;
    j["enableActionOptions"] = spec.enableActionOptions;
    j["textOptions"] = toJson(spec.textOptions);
    j["namePathOptions"] = toJson(spec.namePathOptions);
    j["timeOptions"] = toJson(spec.timeOptions);
    j["sizeOptions"] = toJson(spec.sizeOptions);
    j["typeOptions"] = toJson(spec.typeOptions);
    j["permissionOptions"] = toJson(spec.permissionOptions);
    j["traversalOptions"] = toJson(spec.traversalOptions);
    j["actionOptions"] = toJson(spec.actionOptions);
    return j;
}

bool fromJson(const nlohmann::json &j, SearchSpecification &spec)
{
    try
    {
        if (j.contains("specName"))
            assignToArray(spec.specName, j.value("specName", ""));
        if (j.contains("startLocation"))
            assignToArray(spec.startLocation, j.value("startLocation", ""));
        if (j.contains("searchText"))
            assignToArray(spec.searchText, j.value("searchText", ""));
        if (j.contains("includePatterns"))
            assignToArray(spec.includePatterns, j.value("includePatterns", ""));
        if (j.contains("excludePatterns"))
            assignToArray(spec.excludePatterns, j.value("excludePatterns", ""));
        spec.includeSubdirectories = j.value("includeSubdirectories", spec.includeSubdirectories);
        spec.includeHidden = j.value("includeHidden", spec.includeHidden);
        spec.followSymlinks = j.value("followSymlinks", spec.followSymlinks);
        spec.stayOnSameFilesystem = j.value("stayOnSameFilesystem", spec.stayOnSameFilesystem);
        spec.enableTextSearch = j.value("enableTextSearch", spec.enableTextSearch);
        spec.enableNamePathTests = j.value("enableNamePathTests", spec.enableNamePathTests);
        spec.enableTimeFilters = j.value("enableTimeFilters", spec.enableTimeFilters);
        spec.enableSizeFilters = j.value("enableSizeFilters", spec.enableSizeFilters);
        spec.enableTypeFilters = j.value("enableTypeFilters", spec.enableTypeFilters);
        spec.enablePermissionOwnership = j.value("enablePermissionOwnership", spec.enablePermissionOwnership);
        spec.enableTraversalFilters = j.value("enableTraversalFilters", spec.enableTraversalFilters);
        spec.enableActionOptions = j.value("enableActionOptions", spec.enableActionOptions);
        if (j.contains("textOptions"))
            fromJson(j.at("textOptions"), spec.textOptions);
        if (j.contains("namePathOptions"))
            fromJson(j.at("namePathOptions"), spec.namePathOptions);
        if (j.contains("timeOptions"))
            fromJson(j.at("timeOptions"), spec.timeOptions);
        if (j.contains("sizeOptions"))
            fromJson(j.at("sizeOptions"), spec.sizeOptions);
        if (j.contains("typeOptions"))
            fromJson(j.at("typeOptions"), spec.typeOptions);
        if (j.contains("permissionOptions"))
            fromJson(j.at("permissionOptions"), spec.permissionOptions);
        if (j.contains("traversalOptions"))
            fromJson(j.at("traversalOptions"), spec.traversalOptions);
        if (j.contains("actionOptions"))
            fromJson(j.at("actionOptions"), spec.actionOptions);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

nlohmann::json toJson(const TextSearchOptions &options)
{
    nlohmann::json j;
    j["mode"] = static_cast<int>(options.mode);
    j["matchCase"] = options.matchCase;
    j["searchInContents"] = options.searchInContents;
    j["searchInFileNames"] = options.searchInFileNames;
    j["allowMultipleTerms"] = options.allowMultipleTerms;
    j["treatBinaryAsText"] = options.treatBinaryAsText;
    return j;
}

void fromJson(const nlohmann::json &j, TextSearchOptions &options)
{
    options.mode = static_cast<TextSearchOptions::Mode>(j.value("mode", static_cast<int>(options.mode)));
    options.matchCase = j.value("matchCase", options.matchCase);
    options.searchInContents = j.value("searchInContents", options.searchInContents);
    options.searchInFileNames = j.value("searchInFileNames", options.searchInFileNames);
    options.allowMultipleTerms = j.value("allowMultipleTerms", options.allowMultipleTerms);
    options.treatBinaryAsText = j.value("treatBinaryAsText", options.treatBinaryAsText);
}

nlohmann::json toJson(const NamePathOptions &options)
{
    nlohmann::json j;
    j["nameEnabled"] = options.nameEnabled;
    j["inameEnabled"] = options.inameEnabled;
    j["pathEnabled"] = options.pathEnabled;
    j["ipathEnabled"] = options.ipathEnabled;
    j["regexEnabled"] = options.regexEnabled;
    j["iregexEnabled"] = options.iregexEnabled;
    j["lnameEnabled"] = options.lnameEnabled;
    j["ilnameEnabled"] = options.ilnameEnabled;
    j["pruneEnabled"] = options.pruneEnabled;
    j["pruneDirectoriesOnly"] = options.pruneDirectoriesOnly;
    j["pruneTest"] = static_cast<int>(options.pruneTest);
    j["namePattern"] = arrayToString(options.namePattern);
    j["inamePattern"] = arrayToString(options.inamePattern);
    j["pathPattern"] = arrayToString(options.pathPattern);
    j["ipathPattern"] = arrayToString(options.ipathPattern);
    j["regexPattern"] = arrayToString(options.regexPattern);
    j["iregexPattern"] = arrayToString(options.iregexPattern);
    j["lnamePattern"] = arrayToString(options.lnamePattern);
    j["ilnamePattern"] = arrayToString(options.ilnamePattern);
    j["prunePattern"] = arrayToString(options.prunePattern);
    return j;
}

void fromJson(const nlohmann::json &j, NamePathOptions &options)
{
    options.nameEnabled = j.value("nameEnabled", options.nameEnabled);
    options.inameEnabled = j.value("inameEnabled", options.inameEnabled);
    options.pathEnabled = j.value("pathEnabled", options.pathEnabled);
    options.ipathEnabled = j.value("ipathEnabled", options.ipathEnabled);
    options.regexEnabled = j.value("regexEnabled", options.regexEnabled);
    options.iregexEnabled = j.value("iregexEnabled", options.iregexEnabled);
    options.lnameEnabled = j.value("lnameEnabled", options.lnameEnabled);
    options.ilnameEnabled = j.value("ilnameEnabled", options.ilnameEnabled);
    options.pruneEnabled = j.value("pruneEnabled", options.pruneEnabled);
    options.pruneDirectoriesOnly = j.value("pruneDirectoriesOnly", options.pruneDirectoriesOnly);
    options.pruneTest = static_cast<NamePathOptions::PruneTest>(j.value("pruneTest", static_cast<int>(options.pruneTest)));
    if (j.contains("namePattern"))
        assignToArray(options.namePattern, j.value("namePattern", ""));
    if (j.contains("inamePattern"))
        assignToArray(options.inamePattern, j.value("inamePattern", ""));
    if (j.contains("pathPattern"))
        assignToArray(options.pathPattern, j.value("pathPattern", ""));
    if (j.contains("ipathPattern"))
        assignToArray(options.ipathPattern, j.value("ipathPattern", ""));
    if (j.contains("regexPattern"))
        assignToArray(options.regexPattern, j.value("regexPattern", ""));
    if (j.contains("iregexPattern"))
        assignToArray(options.iregexPattern, j.value("iregexPattern", ""));
    if (j.contains("lnamePattern"))
        assignToArray(options.lnamePattern, j.value("lnamePattern", ""));
    if (j.contains("ilnamePattern"))
        assignToArray(options.ilnamePattern, j.value("ilnamePattern", ""));
    if (j.contains("prunePattern"))
        assignToArray(options.prunePattern, j.value("prunePattern", ""));
}

nlohmann::json toJson(const TimeFilterOptions &options)
{
    nlohmann::json j;
    j["preset"] = static_cast<int>(options.preset);
    j["includeModified"] = options.includeModified;
    j["includeCreated"] = options.includeCreated;
    j["includeAccessed"] = options.includeAccessed;
    j["customFrom"] = arrayToString(options.customFrom);
    j["customTo"] = arrayToString(options.customTo);
    j["useMTime"] = options.useMTime;
    j["useATime"] = options.useATime;
    j["useCTime"] = options.useCTime;
    j["useMMin"] = options.useMMin;
    j["useAMin"] = options.useAMin;
    j["useCMin"] = options.useCMin;
    j["useUsed"] = options.useUsed;
    j["useNewer"] = options.useNewer;
    j["useANewer"] = options.useANewer;
    j["useCNewer"] = options.useCNewer;
    j["useNewermt"] = options.useNewermt;
    j["useNewerat"] = options.useNewerat;
    j["useNewerct"] = options.useNewerct;
    j["mtime"] = arrayToString(options.mtime);
    j["atime"] = arrayToString(options.atime);
    j["ctime"] = arrayToString(options.ctime);
    j["mmin"] = arrayToString(options.mmin);
    j["amin"] = arrayToString(options.amin);
    j["cmin"] = arrayToString(options.cmin);
    j["used"] = arrayToString(options.used);
    j["newer"] = arrayToString(options.newer);
    j["anewer"] = arrayToString(options.anewer);
    j["cnewer"] = arrayToString(options.cnewer);
    j["newermt"] = arrayToString(options.newermt);
    j["newerat"] = arrayToString(options.newerat);
    j["newerct"] = arrayToString(options.newerct);
    return j;
}

void fromJson(const nlohmann::json &j, TimeFilterOptions &options)
{
    options.preset = static_cast<TimeFilterOptions::Preset>(j.value("preset", static_cast<int>(options.preset)));
    options.includeModified = j.value("includeModified", options.includeModified);
    options.includeCreated = j.value("includeCreated", options.includeCreated);
    options.includeAccessed = j.value("includeAccessed", options.includeAccessed);
    if (j.contains("customFrom"))
        assignToArray(options.customFrom, j.value("customFrom", ""));
    if (j.contains("customTo"))
        assignToArray(options.customTo, j.value("customTo", ""));
    options.useMTime = j.value("useMTime", options.useMTime);
    options.useATime = j.value("useATime", options.useATime);
    options.useCTime = j.value("useCTime", options.useCTime);
    options.useMMin = j.value("useMMin", options.useMMin);
    options.useAMin = j.value("useAMin", options.useAMin);
    options.useCMin = j.value("useCMin", options.useCMin);
    options.useUsed = j.value("useUsed", options.useUsed);
    options.useNewer = j.value("useNewer", options.useNewer);
    options.useANewer = j.value("useANewer", options.useANewer);
    options.useCNewer = j.value("useCNewer", options.useCNewer);
    options.useNewermt = j.value("useNewermt", options.useNewermt);
    options.useNewerat = j.value("useNewerat", options.useNewerat);
    options.useNewerct = j.value("useNewerct", options.useNewerct);
    if (j.contains("mtime"))
        assignToArray(options.mtime, j.value("mtime", ""));
    if (j.contains("atime"))
        assignToArray(options.atime, j.value("atime", ""));
    if (j.contains("ctime"))
        assignToArray(options.ctime, j.value("ctime", ""));
    if (j.contains("mmin"))
        assignToArray(options.mmin, j.value("mmin", ""));
    if (j.contains("amin"))
        assignToArray(options.amin, j.value("amin", ""));
    if (j.contains("cmin"))
        assignToArray(options.cmin, j.value("cmin", ""));
    if (j.contains("used"))
        assignToArray(options.used, j.value("used", ""));
    if (j.contains("newer"))
        assignToArray(options.newer, j.value("newer", ""));
    if (j.contains("anewer"))
        assignToArray(options.anewer, j.value("anewer", ""));
    if (j.contains("cnewer"))
        assignToArray(options.cnewer, j.value("cnewer", ""));
    if (j.contains("newermt"))
        assignToArray(options.newermt, j.value("newermt", ""));
    if (j.contains("newerat"))
        assignToArray(options.newerat, j.value("newerat", ""));
    if (j.contains("newerct"))
        assignToArray(options.newerct, j.value("newerct", ""));
}

nlohmann::json toJson(const SizeFilterOptions &options)
{
    nlohmann::json j;
    j["minEnabled"] = options.minEnabled;
    j["maxEnabled"] = options.maxEnabled;
    j["exactEnabled"] = options.exactEnabled;
    j["rangeInclusive"] = options.rangeInclusive;
    j["includeZeroByte"] = options.includeZeroByte;
    j["treatDirectoriesAsFiles"] = options.treatDirectoriesAsFiles;
    j["useDecimalUnits"] = options.useDecimalUnits;
    j["emptyEnabled"] = options.emptyEnabled;
    j["minSpec"] = arrayToString(options.minSpec);
    j["maxSpec"] = arrayToString(options.maxSpec);
    j["exactSpec"] = arrayToString(options.exactSpec);
    return j;
}

void fromJson(const nlohmann::json &j, SizeFilterOptions &options)
{
    options.minEnabled = j.value("minEnabled", options.minEnabled);
    options.maxEnabled = j.value("maxEnabled", options.maxEnabled);
    options.exactEnabled = j.value("exactEnabled", options.exactEnabled);
    options.rangeInclusive = j.value("rangeInclusive", options.rangeInclusive);
    options.includeZeroByte = j.value("includeZeroByte", options.includeZeroByte);
    options.treatDirectoriesAsFiles = j.value("treatDirectoriesAsFiles", options.treatDirectoriesAsFiles);
    options.useDecimalUnits = j.value("useDecimalUnits", options.useDecimalUnits);
    options.emptyEnabled = j.value("emptyEnabled", options.emptyEnabled);
    if (j.contains("minSpec"))
        assignToArray(options.minSpec, j.value("minSpec", ""));
    if (j.contains("maxSpec"))
        assignToArray(options.maxSpec, j.value("maxSpec", ""));
    if (j.contains("exactSpec"))
        assignToArray(options.exactSpec, j.value("exactSpec", ""));
}

nlohmann::json toJson(const TypeFilterOptions &options)
{
    nlohmann::json j;
    j["typeEnabled"] = options.typeEnabled;
    j["xtypeEnabled"] = options.xtypeEnabled;
    j["useExtensions"] = options.useExtensions;
    j["extensionCaseInsensitive"] = options.extensionCaseInsensitive;
    j["useDetectors"] = options.useDetectors;
    j["typeLetters"] = arrayToString(options.typeLetters);
    j["xtypeLetters"] = arrayToString(options.xtypeLetters);
    j["extensions"] = arrayToString(options.extensions);
    j["detectorTags"] = arrayToString(options.detectorTags);
    return j;
}

void fromJson(const nlohmann::json &j, TypeFilterOptions &options)
{
    options.typeEnabled = j.value("typeEnabled", options.typeEnabled);
    options.xtypeEnabled = j.value("xtypeEnabled", options.xtypeEnabled);
    options.useExtensions = j.value("useExtensions", options.useExtensions);
    options.extensionCaseInsensitive = j.value("extensionCaseInsensitive", options.extensionCaseInsensitive);
    options.useDetectors = j.value("useDetectors", options.useDetectors);
    if (j.contains("typeLetters"))
        assignToArray(options.typeLetters, j.value("typeLetters", ""));
    if (j.contains("xtypeLetters"))
        assignToArray(options.xtypeLetters, j.value("xtypeLetters", ""));
    if (j.contains("extensions"))
        assignToArray(options.extensions, j.value("extensions", ""));
    if (j.contains("detectorTags"))
        assignToArray(options.detectorTags, j.value("detectorTags", ""));
}

nlohmann::json toJson(const PermissionOwnershipOptions &options)
{
    nlohmann::json j;
    j["permEnabled"] = options.permEnabled;
    j["readable"] = options.readable;
    j["writable"] = options.writable;
    j["executable"] = options.executable;
    j["permMode"] = static_cast<int>(options.permMode);
    j["permSpec"] = arrayToString(options.permSpec);
    j["userEnabled"] = options.userEnabled;
    j["uidEnabled"] = options.uidEnabled;
    j["groupEnabled"] = options.groupEnabled;
    j["gidEnabled"] = options.gidEnabled;
    j["noUser"] = options.noUser;
    j["noGroup"] = options.noGroup;
    j["user"] = arrayToString(options.user);
    j["uid"] = arrayToString(options.uid);
    j["group"] = arrayToString(options.group);
    j["gid"] = arrayToString(options.gid);
    return j;
}

void fromJson(const nlohmann::json &j, PermissionOwnershipOptions &options)
{
    options.permEnabled = j.value("permEnabled", options.permEnabled);
    options.readable = j.value("readable", options.readable);
    options.writable = j.value("writable", options.writable);
    options.executable = j.value("executable", options.executable);
    options.permMode = static_cast<PermissionOwnershipOptions::PermMode>(j.value("permMode", static_cast<int>(options.permMode)));
    if (j.contains("permSpec"))
        assignToArray(options.permSpec, j.value("permSpec", ""));
    options.userEnabled = j.value("userEnabled", options.userEnabled);
    options.uidEnabled = j.value("uidEnabled", options.uidEnabled);
    options.groupEnabled = j.value("groupEnabled", options.groupEnabled);
    options.gidEnabled = j.value("gidEnabled", options.gidEnabled);
    options.noUser = j.value("noUser", options.noUser);
    options.noGroup = j.value("noGroup", options.noGroup);
    if (j.contains("user"))
        assignToArray(options.user, j.value("user", ""));
    if (j.contains("uid"))
        assignToArray(options.uid, j.value("uid", ""));
    if (j.contains("group"))
        assignToArray(options.group, j.value("group", ""));
    if (j.contains("gid"))
        assignToArray(options.gid, j.value("gid", ""));
}

nlohmann::json toJson(const TraversalFilesystemOptions &options)
{
    nlohmann::json j;
    j["symlinkMode"] = static_cast<int>(options.symlinkMode);
    j["warningMode"] = static_cast<int>(options.warningMode);
    j["depthFirst"] = options.depthFirst;
    j["stayOnFilesystem"] = options.stayOnFilesystem;
    j["assumeNoLeaf"] = options.assumeNoLeaf;
    j["ignoreReaddirRace"] = options.ignoreReaddirRace;
    j["dayStart"] = options.dayStart;
    j["maxDepthEnabled"] = options.maxDepthEnabled;
    j["minDepthEnabled"] = options.minDepthEnabled;
    j["filesFromEnabled"] = options.filesFromEnabled;
    j["filesFromNullSeparated"] = options.filesFromNullSeparated;
    j["fstypeEnabled"] = options.fstypeEnabled;
    j["linksEnabled"] = options.linksEnabled;
    j["sameFileEnabled"] = options.sameFileEnabled;
    j["inumEnabled"] = options.inumEnabled;
    j["maxDepth"] = arrayToString(options.maxDepth);
    j["minDepth"] = arrayToString(options.minDepth);
    j["filesFrom"] = arrayToString(options.filesFrom);
    j["fsType"] = arrayToString(options.fsType);
    j["linkCount"] = arrayToString(options.linkCount);
    j["sameFile"] = arrayToString(options.sameFile);
    j["inode"] = arrayToString(options.inode);
    return j;
}

void fromJson(const nlohmann::json &j, TraversalFilesystemOptions &options)
{
    options.symlinkMode = static_cast<TraversalFilesystemOptions::SymlinkMode>(j.value("symlinkMode", static_cast<int>(options.symlinkMode)));
    options.warningMode = static_cast<TraversalFilesystemOptions::WarningMode>(j.value("warningMode", static_cast<int>(options.warningMode)));
    options.depthFirst = j.value("depthFirst", options.depthFirst);
    options.stayOnFilesystem = j.value("stayOnFilesystem", options.stayOnFilesystem);
    options.assumeNoLeaf = j.value("assumeNoLeaf", options.assumeNoLeaf);
    options.ignoreReaddirRace = j.value("ignoreReaddirRace", options.ignoreReaddirRace);
    options.dayStart = j.value("dayStart", options.dayStart);
    options.maxDepthEnabled = j.value("maxDepthEnabled", options.maxDepthEnabled);
    options.minDepthEnabled = j.value("minDepthEnabled", options.minDepthEnabled);
    options.filesFromEnabled = j.value("filesFromEnabled", options.filesFromEnabled);
    options.filesFromNullSeparated = j.value("filesFromNullSeparated", options.filesFromNullSeparated);
    options.fstypeEnabled = j.value("fstypeEnabled", options.fstypeEnabled);
    options.linksEnabled = j.value("linksEnabled", options.linksEnabled);
    options.sameFileEnabled = j.value("sameFileEnabled", options.sameFileEnabled);
    options.inumEnabled = j.value("inumEnabled", options.inumEnabled);
    if (j.contains("maxDepth"))
        assignToArray(options.maxDepth, j.value("maxDepth", ""));
    if (j.contains("minDepth"))
        assignToArray(options.minDepth, j.value("minDepth", ""));
    if (j.contains("filesFrom"))
        assignToArray(options.filesFrom, j.value("filesFrom", ""));
    if (j.contains("fsType"))
        assignToArray(options.fsType, j.value("fsType", ""));
    if (j.contains("linkCount"))
        assignToArray(options.linkCount, j.value("linkCount", ""));
    if (j.contains("sameFile"))
        assignToArray(options.sameFile, j.value("sameFile", ""));
    if (j.contains("inode"))
        assignToArray(options.inode, j.value("inode", ""));
}

nlohmann::json toJson(const ActionOptions &options)
{
    nlohmann::json j;
    j["print"] = options.print;
    j["print0"] = options.print0;
    j["ls"] = options.ls;
    j["deleteMatches"] = options.deleteMatches;
    j["quitEarly"] = options.quitEarly;
    j["execEnabled"] = options.execEnabled;
    j["execUsePlus"] = options.execUsePlus;
    j["execVariant"] = static_cast<int>(options.execVariant);
    j["fprintEnabled"] = options.fprintEnabled;
    j["fprintAppend"] = options.fprintAppend;
    j["fprint0Enabled"] = options.fprint0Enabled;
    j["fprint0Append"] = options.fprint0Append;
    j["flsEnabled"] = options.flsEnabled;
    j["flsAppend"] = options.flsAppend;
    j["printfEnabled"] = options.printfEnabled;
    j["fprintfEnabled"] = options.fprintfEnabled;
    j["fprintfAppend"] = options.fprintfAppend;
    j["execCommand"] = arrayToString(options.execCommand);
    j["fprintFile"] = arrayToString(options.fprintFile);
    j["fprint0File"] = arrayToString(options.fprint0File);
    j["flsFile"] = arrayToString(options.flsFile);
    j["printfFormat"] = arrayToString(options.printfFormat);
    j["fprintfFile"] = arrayToString(options.fprintfFile);
    j["fprintfFormat"] = arrayToString(options.fprintfFormat);
    return j;
}

void fromJson(const nlohmann::json &j, ActionOptions &options)
{
    options.print = j.value("print", options.print);
    options.print0 = j.value("print0", options.print0);
    options.ls = j.value("ls", options.ls);
    options.deleteMatches = j.value("deleteMatches", options.deleteMatches);
    options.quitEarly = j.value("quitEarly", options.quitEarly);
    options.execEnabled = j.value("execEnabled", options.execEnabled);
    options.execUsePlus = j.value("execUsePlus", options.execUsePlus);
    options.execVariant = static_cast<ActionOptions::ExecVariant>(j.value("execVariant", static_cast<int>(options.execVariant)));
    options.fprintEnabled = j.value("fprintEnabled", options.fprintEnabled);
    options.fprintAppend = j.value("fprintAppend", options.fprintAppend);
    options.fprint0Enabled = j.value("fprint0Enabled", options.fprint0Enabled);
    options.fprint0Append = j.value("fprint0Append", options.fprint0Append);
    options.flsEnabled = j.value("flsEnabled", options.flsEnabled);
    options.flsAppend = j.value("flsAppend", options.flsAppend);
    options.printfEnabled = j.value("printfEnabled", options.printfEnabled);
    options.fprintfEnabled = j.value("fprintfEnabled", options.fprintfEnabled);
    options.fprintfAppend = j.value("fprintfAppend", options.fprintfAppend);
    if (j.contains("execCommand"))
        assignToArray(options.execCommand, j.value("execCommand", ""));
    if (j.contains("fprintFile"))
        assignToArray(options.fprintFile, j.value("fprintFile", ""));
    if (j.contains("fprint0File"))
        assignToArray(options.fprint0File, j.value("fprint0File", ""));
    if (j.contains("flsFile"))
        assignToArray(options.flsFile, j.value("flsFile", ""));
    if (j.contains("printfFormat"))
        assignToArray(options.printfFormat, j.value("printfFormat", ""));
    if (j.contains("fprintfFile"))
        assignToArray(options.fprintfFile, j.value("fprintfFile", ""));
    if (j.contains("fprintfFormat"))
        assignToArray(options.fprintfFormat, j.value("fprintfFormat", ""));
}

std::filesystem::path specificationPathForSlug(const std::string &slug)
{
    return storageDirectory() / (slug + ".json");
}

std::optional<SearchSpecification> readSpecification(const std::filesystem::path &file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
        return std::nullopt;
    nlohmann::json j;
    try
    {
        stream >> j;
    }
    catch (...)
    {
        return std::nullopt;
    }
    SearchSpecification spec = makeDefaultSpecification();
    if (!fromJson(j, spec))
        return std::nullopt;
    return spec;
}

bool writeSpecification(const std::filesystem::path &file, const SearchSpecification &spec)
{
    std::ofstream stream(file);
    if (!stream.is_open())
        return false;
    nlohmann::json j = toJson(spec);
    stream << j.dump(2) << std::endl;
    return stream.good();
}

std::vector<std::string> tokenizeTerms(const std::string &input)
{
    std::vector<std::string> tokens;
    std::string current;
    std::istringstream stream(input);
    while (stream >> current)
        tokens.push_back(current);
    return tokens;
}

std::vector<std::string> splitExtensions(const std::string &value)
{
    std::vector<std::string> list = splitList(value, ',');
    std::vector<std::string> refined;
    for (const auto &item : list)
    {
        auto pieces = splitList(item, ' ');
        refined.insert(refined.end(), pieces.begin(), pieces.end());
    }
    return refined;
}

void ensurePrintAction(ActionOptions &actions)
{
    if (!actions.print && !actions.print0 && !actions.ls &&
        !actions.deleteMatches && !actions.execEnabled && !actions.fprintEnabled &&
        !actions.fprint0Enabled && !actions.flsEnabled && !actions.printfEnabled &&
        !actions.fprintfEnabled)
    {
        actions.print = true;
    }
}

void addOrGroup(std::vector<std::string> &target, const std::vector<std::vector<std::string>> &expressions)
{
    if (expressions.empty())
        return;
    if (expressions.size() == 1)
    {
        target.insert(target.end(), expressions.front().begin(), expressions.front().end());
        return;
    }
    target.emplace_back("(");
    for (std::size_t i = 0; i < expressions.size(); ++i)
    {
        const auto &expr = expressions[i];
        target.insert(target.end(), expr.begin(), expr.end());
        if (i + 1 < expressions.size())
            target.emplace_back("-o");
    }
    target.emplace_back(")");
}

void addAndGroup(std::vector<std::string> &target, const std::vector<std::vector<std::string>> &expressions)
{
    for (const auto &expr : expressions)
        target.insert(target.end(), expr.begin(), expr.end());
}

std::string sanitiseCommandToken(const std::string &token)
{
    return token;
}

std::vector<std::string> splitCommand(const std::string &command)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;
    for (std::size_t i = 0; i < command.size(); ++i)
    {
        char ch = command[i];
        if (ch == '\'' && !inDouble)
        {
            if (inSingle)
            {
                tokens.push_back(current);
                current.clear();
                inSingle = false;
            }
            else
            {
                if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }
                inSingle = true;
            }
            continue;
        }
        if (ch == '"' && !inSingle)
        {
            if (inDouble)
            {
                tokens.push_back(current);
                current.clear();
                inDouble = false;
            }
            else
            {
                if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }
                inDouble = true;
            }
            continue;
        }
        if (!inSingle && !inDouble && std::isspace(static_cast<unsigned char>(ch)))
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        if (ch == '\\' && i + 1 < command.size())
        {
            current.push_back(command[++i]);
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty())
        tokens.push_back(current);
    return tokens;
}

std::vector<std::string> parseStartLocations(const SearchSpecification &spec)
{
    std::string raw = trimCopy(arrayToString(spec.startLocation));
    if (raw.empty())
        return {"."};

    std::replace(raw.begin(), raw.end(), '\n', ';');
    std::replace(raw.begin(), raw.end(), '\r', ';');
    std::vector<std::string> paths;
    std::string token;
    std::stringstream stream(raw);
    while (std::getline(stream, token, ';'))
    {
        auto chunk = trimCopy(token);
        if (!chunk.empty())
            paths.push_back(chunk);
    }

    if (paths.empty())
        paths.push_back(".");

    return paths;
}

void applySymlinkMode(const TraversalFilesystemOptions &options, std::vector<std::string> &args)
{
    switch (options.symlinkMode)
    {
    case TraversalFilesystemOptions::SymlinkMode::CommandLine:
        args.emplace_back("-H");
        break;
    case TraversalFilesystemOptions::SymlinkMode::Everywhere:
        args.emplace_back("-L");
        break;
    case TraversalFilesystemOptions::SymlinkMode::Physical:
    default:
        args.emplace_back("-P");
        break;
    }
}

int waitForChild(pid_t pid)
{
    int status = 0;
    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno != EINTR)
        {
            return -1;
        }
    }
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return status;
}

SearchExecutionResult executeCommand(const std::vector<std::string> &command,
                                     bool captureMatches,
                                     std::ostream *forwardStdout,
                                     std::ostream *forwardStderr)
{
    SearchExecutionResult result;
    result.command = command;

    bool interceptStdout = captureMatches || forwardStdout != nullptr;
    int stdoutPipe[2]{-1, -1};
    if (interceptStdout)
    {
        if (pipe(stdoutPipe) == -1)
        {
            result.exitCode = -1;
            return result;
        }
    }

    bool interceptStderr = forwardStderr != nullptr;
    int stderrPipe[2]{-1, -1};
    if (interceptStderr)
    {
        if (pipe(stderrPipe) == -1)
        {
            if (interceptStdout)
            {
                close(stdoutPipe[0]);
                close(stdoutPipe[1]);
            }
            result.exitCode = -1;
            return result;
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (interceptStdout)
    {
        posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
        posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
    }
    if (interceptStderr)
    {
        posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
        posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);
    }

    std::vector<char *> argv;
    argv.reserve(command.size() + 1);
    for (const auto &token : command)
        argv.push_back(const_cast<char *>(token.c_str()));
    argv.push_back(nullptr);

    pid_t childPid = -1;
    int spawnStatus = posix_spawnp(&childPid, command.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (interceptStdout)
        close(stdoutPipe[1]);
    if (interceptStderr)
        close(stderrPipe[1]);

    if (spawnStatus != 0)
    {
        if (interceptStdout)
            close(stdoutPipe[0]);
        if (interceptStderr)
            close(stderrPipe[0]);
        result.exitCode = spawnStatus;
        return result;
    }

    constexpr std::size_t kBufferSize = 8192;
    std::array<char, kBufferSize> buffer{};
    std::string pending;

    if (interceptStdout)
    {
        ssize_t bytesRead = 0;
        while ((bytesRead = read(stdoutPipe[0], buffer.data(), buffer.size())) > 0)
        {
            if (!captureMatches && forwardStdout)
                forwardStdout->write(buffer.data(), bytesRead);
            if (captureMatches)
                pending.append(buffer.data(), static_cast<std::size_t>(bytesRead));
        }
        close(stdoutPipe[0]);
    }

    if (interceptStderr)
    {
        ssize_t bytesRead = 0;
        while ((bytesRead = read(stderrPipe[0], buffer.data(), buffer.size())) > 0)
            forwardStderr->write(buffer.data(), bytesRead);
        close(stderrPipe[0]);
    }

    result.exitCode = waitForChild(childPid);

    if (captureMatches && !pending.empty())
    {
        std::stringstream stream(pending);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                result.matches.emplace_back(line);
        }
    }

    return result;
}

std::vector<std::string> buildActionTokens(const ActionOptions &options)
{
    std::vector<std::string> tokens;
    if (options.print)
        tokens.emplace_back("-print");
    if (options.print0)
        tokens.emplace_back("-print0");
    if (options.ls)
        tokens.emplace_back("-ls");
    if (options.deleteMatches)
        tokens.emplace_back("-delete");
    if (options.quitEarly)
        tokens.emplace_back("-quit");
    if (options.execEnabled)
    {
        std::vector<std::string> execTokens = splitCommand(arrayToString(options.execCommand));
        if (!execTokens.empty())
        {
            switch (options.execVariant)
            {
            case ActionOptions::ExecVariant::ExecDir:
                tokens.emplace_back("-execdir");
                break;
            case ActionOptions::ExecVariant::Ok:
                tokens.emplace_back("-ok");
                break;
            case ActionOptions::ExecVariant::OkDir:
                tokens.emplace_back("-okdir");
                break;
            case ActionOptions::ExecVariant::Exec:
            default:
                tokens.emplace_back("-exec");
                break;
            }
            tokens.insert(tokens.end(), execTokens.begin(), execTokens.end());
            tokens.emplace_back(options.execUsePlus ? "+" : ";");
        }
    }
    if (options.fprintEnabled)
    {
        tokens.emplace_back("-fprint");
        tokens.emplace_back(arrayToString(options.fprintFile));
    }
    if (options.fprint0Enabled)
    {
        tokens.emplace_back("-fprint0");
        tokens.emplace_back(arrayToString(options.fprint0File));
    }
    if (options.flsEnabled)
    {
        tokens.emplace_back("-fls");
        tokens.emplace_back(arrayToString(options.flsFile));
    }
    if (options.printfEnabled)
    {
        tokens.emplace_back("-printf");
        tokens.emplace_back(arrayToString(options.printfFormat));
    }
    if (options.fprintfEnabled)
    {
        tokens.emplace_back("-fprintf");
        tokens.emplace_back(arrayToString(options.fprintfFile));
        tokens.emplace_back(arrayToString(options.fprintfFormat));
    }
    return tokens;
}

std::vector<std::string> nameTestTokens(const NamePathOptions &options, bool caseInsensitive, const std::string &pattern, const std::string &flag)
{
    if (pattern.empty())
        return {};
    std::vector<std::string> tokens;
    tokens.emplace_back(flag);
    tokens.emplace_back(pattern);
    return tokens;
}

std::vector<std::string> pruneExpression(const NamePathOptions &options)
{
    if (!options.pruneEnabled || arrayToString(options.prunePattern).empty())
        return {};
    std::string pattern = arrayToString(options.prunePattern);
    std::vector<std::string> expr;
    switch (options.pruneTest)
    {
    case NamePathOptions::PruneTest::Name:
        expr = {"-name", pattern};
        break;
    case NamePathOptions::PruneTest::Iname:
        expr = {"-iname", pattern};
        break;
    case NamePathOptions::PruneTest::Path:
        expr = {"-path", pattern};
        break;
    case NamePathOptions::PruneTest::Ipath:
        expr = {"-ipath", pattern};
        break;
    case NamePathOptions::PruneTest::Regex:
        expr = {"-regex", pattern};
        break;
    case NamePathOptions::PruneTest::Iregex:
        expr = {"-iregex", pattern};
        break;
    }
    return expr;
}

void addTimePresetExpressions(std::vector<std::string> &tests, const TimeFilterOptions &options)
{
    auto addPreset = [&](const std::string &flag, int days) {
        tests.emplace_back(flag);
        tests.emplace_back("-" + std::to_string(days));
    };

    int days = 0;
    switch (options.preset)
    {
    case TimeFilterOptions::Preset::PastDay:
        days = 1;
        break;
    case TimeFilterOptions::Preset::PastWeek:
        days = 7;
        break;
    case TimeFilterOptions::Preset::PastMonth:
        days = 30;
        break;
    case TimeFilterOptions::Preset::PastSixMonths:
        days = 182;
        break;
    case TimeFilterOptions::Preset::PastYear:
        days = 365;
        break;
    case TimeFilterOptions::Preset::PastSixYears:
        days = 365 * 6;
        break;
    case TimeFilterOptions::Preset::CustomRange:
    case TimeFilterOptions::Preset::AnyTime:
    default:
        break;
    }
    if (days <= 0)
        return;

    if (options.includeModified)
        addPreset("-mtime", days);
    if (options.includeCreated)
        addPreset("-ctime", days);
    if (options.includeAccessed)
        addPreset("-atime", days);
}

void addCustomRangeExpressions(std::vector<std::string> &tests, const TimeFilterOptions &options)
{
    if (options.preset != TimeFilterOptions::Preset::CustomRange)
        return;
    std::string from = trimCopy(arrayToString(options.customFrom));
    std::string to = trimCopy(arrayToString(options.customTo));
    if (from.empty() && to.empty())
        return;

    auto addRange = [&](const std::string &newerFlag, const std::string &upperFlag) {
        if (!from.empty())
        {
            tests.emplace_back(newerFlag);
            tests.emplace_back(from);
        }
        if (!to.empty())
        {
            tests.emplace_back("!");
            tests.emplace_back(upperFlag);
            tests.emplace_back(to);
        }
    };

    if (options.includeModified)
        addRange("-newermt", "-newermt");
    if (options.includeCreated)
        addRange("-newerct", "-newerct");
    if (options.includeAccessed)
        addRange("-newerat", "-newerat");
}

void addManualTimeExpressions(std::vector<std::string> &tests, const TimeFilterOptions &options)
{
    auto addIf = [&](bool enabled, const std::string &flag, const std::array<char, 16> &value) {
        if (enabled && value[0] != '\0')
        {
            tests.emplace_back(flag);
            tests.emplace_back(arrayToString(value));
        }
    };
    auto addIfPath = [&](bool enabled, const std::string &flag, const std::array<char, PATH_MAX> &value) {
        if (enabled && value[0] != '\0')
        {
            tests.emplace_back(flag);
            tests.emplace_back(arrayToString(value));
        }
    };
    auto addIfTimestamp = [&](bool enabled, const std::string &flag, const std::array<char, 64> &value) {
        if (enabled && value[0] != '\0')
        {
            tests.emplace_back(flag);
            tests.emplace_back(arrayToString(value));
        }
    };

    addIf(options.useMTime, "-mtime", options.mtime);
    addIf(options.useATime, "-atime", options.atime);
    addIf(options.useCTime, "-ctime", options.ctime);
    addIf(options.useMMin, "-mmin", options.mmin);
    addIf(options.useAMin, "-amin", options.amin);
    addIf(options.useCMin, "-cmin", options.cmin);
    addIf(options.useUsed, "-used", options.used);

    addIfPath(options.useNewer, "-newer", options.newer);
    addIfPath(options.useANewer, "-anewer", options.anewer);
    addIfPath(options.useCNewer, "-cnewer", options.cnewer);
    addIfTimestamp(options.useNewermt, "-newermt", options.newermt);
    addIfTimestamp(options.useNewerat, "-newerat", options.newerat);
    addIfTimestamp(options.useNewerct, "-newerct", options.newerct);
}

void addSizeExpressions(std::vector<std::string> &tests, const SizeFilterOptions &options)
{
    auto prepareSpec = [](const std::array<char, 32> &specValue, bool isMin, bool inclusive) {
        std::string spec = trimCopy(arrayToString(specValue));
        if (spec.empty())
            return spec;
        if (spec.front() == '+' || spec.front() == '-' || spec.front() == '/')
            return spec;
        if (isMin)
            return std::string(inclusive ? "+" : "+") + spec;
        return std::string(inclusive ? "-" : "-") + spec;
    };

    if (options.minEnabled)
    {
        std::string spec = prepareSpec(options.minSpec, true, options.rangeInclusive);
        if (!spec.empty())
        {
            tests.emplace_back("-size");
            tests.emplace_back(spec);
        }
    }
    if (options.maxEnabled)
    {
        std::string spec = prepareSpec(options.maxSpec, false, options.rangeInclusive);
        if (!spec.empty())
        {
            tests.emplace_back("-size");
            tests.emplace_back(spec);
        }
    }
    if (options.exactEnabled)
    {
        std::string spec = trimCopy(arrayToString(options.exactSpec));
        if (!spec.empty())
        {
            tests.emplace_back("-size");
            tests.emplace_back(spec);
        }
    }
    if (options.emptyEnabled)
        tests.emplace_back("-empty");
    if (!options.includeZeroByte)
    {
        tests.emplace_back("!");
        tests.emplace_back("-size");
        tests.emplace_back("0");
    }
}

void addTypeExpressions(std::vector<std::string> &tests, const TypeFilterOptions &options)
{
    std::string types = arrayToString(options.typeLetters);
    std::vector<std::string> typeTokens;
    for (char letter : types)
    {
        std::string flag(1, letter);
        typeTokens.push_back(flag);
    }
    if (options.typeEnabled && !typeTokens.empty())
    {
        std::vector<std::vector<std::string>> exprs;
        for (const auto &token : typeTokens)
            exprs.push_back({"-type", token});
        addOrGroup(tests, exprs);
    }

    std::string xtypes = arrayToString(options.xtypeLetters);
    if (options.xtypeEnabled && !xtypes.empty())
    {
        std::vector<std::vector<std::string>> exprs;
        for (char letter : xtypes)
        {
            std::string flag(1, letter);
            exprs.push_back({"-xtype", flag});
        }
        addOrGroup(tests, exprs);
    }

    if (options.useExtensions && options.extensions[0] != '\0')
    {
        auto extensions = splitExtensions(arrayToString(options.extensions));
        if (!extensions.empty())
        {
            std::vector<std::vector<std::string>> exprs;
            for (const auto &ext : extensions)
            {
                std::string pattern = "*." + ext;
                exprs.push_back({options.extensionCaseInsensitive ? "-iname" : "-name", pattern});
            }
            addOrGroup(tests, exprs);
        }
    }
}

void addPermissionExpressions(std::vector<std::string> &tests, const PermissionOwnershipOptions &options)
{
    if (options.readable)
        tests.emplace_back("-readable");
    if (options.writable)
        tests.emplace_back("-writable");
    if (options.executable)
        tests.emplace_back("-executable");
    if (options.permEnabled && options.permSpec[0] != '\0')
    {
        std::string spec = arrayToString(options.permSpec);
        switch (options.permMode)
        {
        case PermissionOwnershipOptions::PermMode::Exact:
            tests.emplace_back("-perm");
            tests.emplace_back(spec);
            break;
        case PermissionOwnershipOptions::PermMode::AllBits:
            tests.emplace_back("-perm");
            tests.emplace_back("-" + spec);
            break;
        case PermissionOwnershipOptions::PermMode::AnyBit:
            tests.emplace_back("-perm");
            tests.emplace_back("/" + spec);
            break;
        }
    }
    if (options.userEnabled && options.user[0] != '\0')
    {
        tests.emplace_back("-user");
        tests.emplace_back(arrayToString(options.user));
    }
    if (options.uidEnabled && options.uid[0] != '\0')
    {
        tests.emplace_back("-uid");
        tests.emplace_back(arrayToString(options.uid));
    }
    if (options.groupEnabled && options.group[0] != '\0')
    {
        tests.emplace_back("-group");
        tests.emplace_back(arrayToString(options.group));
    }
    if (options.gidEnabled && options.gid[0] != '\0')
    {
        tests.emplace_back("-gid");
        tests.emplace_back(arrayToString(options.gid));
    }
    if (options.noUser)
        tests.emplace_back("-nouser");
    if (options.noGroup)
        tests.emplace_back("-nogroup");
}

void addTraversalExpressions(std::vector<std::string> &args,
                             const TraversalFilesystemOptions &options,
                             bool includeSubdirectories)
{
    applySymlinkMode(options, args);

    switch (options.warningMode)
    {
    case TraversalFilesystemOptions::WarningMode::ForceWarn:
        args.emplace_back("-warn");
        break;
    case TraversalFilesystemOptions::WarningMode::SuppressWarn:
        args.emplace_back("-nowarn");
        break;
    case TraversalFilesystemOptions::WarningMode::Default:
    default:
        break;
    }

    if (options.depthFirst)
        args.emplace_back("-depth");
    if (options.stayOnFilesystem)
        args.emplace_back("-xdev");
    if (options.assumeNoLeaf)
        args.emplace_back("-noleaf");
    if (options.ignoreReaddirRace)
        args.emplace_back("-ignore_readdir_race");
    if (options.dayStart)
        args.emplace_back("-daystart");

    if (options.filesFromEnabled && options.filesFrom[0] != '\0')
    {
        args.emplace_back(options.filesFromNullSeparated ? "-files0-from" : "-files-from");
        args.emplace_back(arrayToString(options.filesFrom));
    }
    if (options.fstypeEnabled && options.fsType[0] != '\0')
    {
        args.emplace_back("-fstype");
        args.emplace_back(arrayToString(options.fsType));
    }
    if (options.linksEnabled && options.linkCount[0] != '\0')
    {
        args.emplace_back("-links");
        args.emplace_back(arrayToString(options.linkCount));
    }
    if (options.sameFileEnabled && options.sameFile[0] != '\0')
    {
        args.emplace_back("-samefile");
        args.emplace_back(arrayToString(options.sameFile));
    }
    if (options.inumEnabled && options.inode[0] != '\0')
    {
        args.emplace_back("-inum");
        args.emplace_back(arrayToString(options.inode));
    }

    if (options.maxDepthEnabled && options.maxDepth[0] != '\0')
    {
        args.emplace_back("-maxdepth");
        args.emplace_back(arrayToString(options.maxDepth));
    }
    else if (!includeSubdirectories)
    {
        args.emplace_back("-maxdepth");
        args.emplace_back("1");
    }

    if (options.minDepthEnabled && options.minDepth[0] != '\0')
    {
        args.emplace_back("-mindepth");
        args.emplace_back(arrayToString(options.minDepth));
    }
}

void addIncludeExcludePatterns(std::vector<std::string> &tests,
                               const SearchSpecification &spec)
{
    auto includePatterns = splitExtensions(arrayToString(spec.includePatterns));
    if (!includePatterns.empty())
    {
        std::vector<std::vector<std::string>> exprs;
        for (const auto &pattern : includePatterns)
            exprs.push_back({"-name", pattern});
        addOrGroup(tests, exprs);
    }

    auto excludePatterns = splitExtensions(arrayToString(spec.excludePatterns));
    for (const auto &pattern : excludePatterns)
    {
        tests.emplace_back("!");
        tests.emplace_back("-name");
        tests.emplace_back(pattern);
    }
}

void addHiddenFilter(std::vector<std::string> &tests, bool includeHidden)
{
    if (includeHidden)
        return;

    std::vector<std::vector<std::string>> exprs;
    exprs.push_back({"!", "-name", ".*"});
    exprs.push_back({"!", "-path", "*/.*"});
    addAndGroup(tests, exprs);
}

std::vector<std::string> buildTextNameExpressions(const SearchSpecification &spec)
{
    std::vector<std::string> tests;
    if (!spec.enableTextSearch)
        return tests;

    std::string searchText = trimCopy(arrayToString(spec.searchText));
    if (searchText.empty())
        return tests;

    const TextSearchOptions &text = spec.textOptions;
    bool caseInsensitive = !text.matchCase;
    auto terms = text.allowMultipleTerms ? tokenizeTerms(searchText) : std::vector<std::string>{searchText};
    if (terms.empty())
        return tests;

    if (text.searchInFileNames)
    {
        std::vector<std::vector<std::string>> exprs;
        for (const auto &term : terms)
        {
            switch (text.mode)
            {
            case TextSearchOptions::Mode::RegularExpression:
            {
                std::string pattern = ".*" + term + ".*";
                exprs.push_back({caseInsensitive ? "-iregex" : "-regex", pattern});
                break;
            }
            case TextSearchOptions::Mode::WholeWord:
            {
                exprs.push_back({caseInsensitive ? "-iname" : "-name", term});
                break;
            }
            case TextSearchOptions::Mode::Contains:
            default:
            {
                std::string pattern = "*" + term + "*";
                exprs.push_back({caseInsensitive ? "-iname" : "-name", pattern});
                break;
            }
            }
        }
        addAndGroup(tests, exprs);
    }
    return tests;
}

bool isBinaryFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;
    constexpr std::size_t kSample = 1024;
    std::array<char, kSample> buffer{};
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    std::streamsize count = file.gcount();
    for (std::streamsize i = 0; i < count; ++i)
    {
        if (buffer[static_cast<std::size_t>(i)] == '\0')
            return true;
    }
    return false;
}

bool matchContains(const std::string &haystack, const std::vector<std::string> &needles, bool caseInsensitive)
{
    auto toLower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    std::string target = caseInsensitive ? toLower(haystack) : haystack;
    for (const auto &needle : needles)
    {
        std::string n = caseInsensitive ? toLower(needle) : needle;
        if (target.find(n) == std::string::npos)
            return false;
    }
    return true;
}

bool matchWholeWord(const std::string &haystack, const std::vector<std::string> &needles, bool caseInsensitive)
{
    std::istringstream stream(haystack);
    std::vector<std::string> words;
    std::string word;
    while (stream >> word)
    {
        if (caseInsensitive)
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        words.push_back(word);
    }
    for (auto needle : needles)
    {
        if (caseInsensitive)
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(words.begin(), words.end(), needle) == words.end())
            return false;
    }
    return true;
}

bool matchRegex(const std::string &haystack, const std::string &pattern, bool caseInsensitive)
{
    try
    {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (caseInsensitive)
            flags = static_cast<std::regex::flag_type>(flags | std::regex::icase);
        std::regex re(pattern, flags);
        return std::regex_search(haystack, re);
    }
    catch (...)
    {
        return false;
    }
}

bool fileMatchesContent(const std::filesystem::path &path,
                        const TextSearchOptions &options,
                        const std::vector<std::string> &terms,
                        const std::string &rawPattern)
{
    if (!options.treatBinaryAsText && isBinaryFile(path))
        return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    switch (options.mode)
    {
    case TextSearchOptions::Mode::RegularExpression:
        return matchRegex(content, rawPattern, !options.matchCase);
    case TextSearchOptions::Mode::WholeWord:
        return matchWholeWord(content, terms, !options.matchCase);
    case TextSearchOptions::Mode::Contains:
    default:
        return matchContains(content, terms, !options.matchCase);
    }
}

} // namespace

std::filesystem::path specificationStorageDirectory()
{
    return storageDirectory();
}

std::vector<SavedSpecification> listSavedSpecifications()
{
    std::vector<SavedSpecification> specs;
    std::filesystem::path dir = storageDirectory();
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file())
            continue;
        auto path = entry.path();
        if (path.extension() != ".json")
            continue;
        auto optSpec = readSpecification(path);
        if (!optSpec)
            continue;
        SavedSpecification info;
        info.name = trimCopy(arrayToString(optSpec->specName));
        if (info.name.empty())
            info.name = path.stem().string();
        info.slug = path.stem().string();
        info.path = path;
        specs.push_back(std::move(info));
    }
    std::sort(specs.begin(), specs.end(), [](const SavedSpecification &a, const SavedSpecification &b) {
        return a.name < b.name;
    });
    return specs;
}

std::optional<SearchSpecification> loadSpecification(const std::string &nameOrSlug)
{
    std::string trimmed = trimCopy(nameOrSlug);
    if (trimmed.empty())
        return std::nullopt;

    std::string slug = slugify(trimmed);
    std::filesystem::path path = specificationPathForSlug(slug);
    if (auto spec = readSpecification(path))
        return spec;

    auto specs = listSavedSpecifications();
    for (const auto &info : specs)
    {
        if (info.name == trimmed)
            return readSpecification(info.path);
    }
    return std::nullopt;
}

bool saveSpecification(const SearchSpecification &spec)
{
    std::string name = trimCopy(arrayToString(spec.specName));
    if (name.empty())
        name = "Unnamed";
    return saveSpecification(spec, name);
}

bool saveSpecification(const SearchSpecification &spec, const std::string &name)
{
    std::string trimmed = trimCopy(name);
    if (trimmed.empty())
        return false;
    std::string slug = slugify(trimmed);
    std::filesystem::path target = specificationPathForSlug(slug);
    SearchSpecification toSave = spec;
    assignToArray(toSave.specName, trimmed);
    ensurePrintAction(toSave.actionOptions);
    return writeSpecification(target, toSave);
}

bool removeSpecification(const std::string &nameOrSlug)
{
    std::string trimmed = trimCopy(nameOrSlug);
    if (trimmed.empty())
        return false;
    std::string slug = slugify(trimmed);
    std::filesystem::path path = specificationPathForSlug(slug);
    std::error_code ec;
    if (std::filesystem::remove(path, ec))
        return true;
    auto specs = listSavedSpecifications();
    for (const auto &info : specs)
    {
        if (info.name == trimmed)
        {
            std::filesystem::remove(info.path, ec);
            return !ec;
        }
    }
    return false;
}

std::string normaliseSpecificationName(const std::string &name)
{
    return trimCopy(name);
}

std::vector<std::string> buildFindCommand(const SearchSpecification &spec, bool includeActions)
{
    std::vector<std::string> args;
    args.emplace_back("find");

    auto startLocations = parseStartLocations(spec);
    args.insert(args.end(), startLocations.begin(), startLocations.end());

    addTraversalExpressions(args, spec.traversalOptions, spec.includeSubdirectories);

    std::vector<std::string> tests;
    addHiddenFilter(tests, spec.includeHidden);
    addIncludeExcludePatterns(tests, spec);

    if (spec.enableNamePathTests)
    {
        const auto &np = spec.namePathOptions;
        if (np.nameEnabled && np.namePattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-name", arrayToString(np.namePattern)});
        if (np.inameEnabled && np.inamePattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-iname", arrayToString(np.inamePattern)});
        if (np.pathEnabled && np.pathPattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-path", arrayToString(np.pathPattern)});
        if (np.ipathEnabled && np.ipathPattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-ipath", arrayToString(np.ipathPattern)});
        if (np.regexEnabled && np.regexPattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-regex", arrayToString(np.regexPattern)});
        if (np.iregexEnabled && np.iregexPattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-iregex", arrayToString(np.iregexPattern)});
        if (np.lnameEnabled && np.lnamePattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-lname", arrayToString(np.lnamePattern)});
        if (np.ilnameEnabled && np.ilnamePattern[0] != '\0')
            tests.insert(tests.end(), std::initializer_list<std::string>{"-ilname", arrayToString(np.ilnamePattern)});
    }

    auto textTests = buildTextNameExpressions(spec);
    tests.insert(tests.end(), textTests.begin(), textTests.end());

    if (spec.enableTimeFilters)
    {
        addTimePresetExpressions(tests, spec.timeOptions);
        addCustomRangeExpressions(tests, spec.timeOptions);
        addManualTimeExpressions(tests, spec.timeOptions);
    }

    if (spec.enableSizeFilters)
        addSizeExpressions(tests, spec.sizeOptions);

    if (spec.enableTypeFilters)
        addTypeExpressions(tests, spec.typeOptions);

    if (spec.enablePermissionOwnership)
        addPermissionExpressions(tests, spec.permissionOptions);

    if (spec.namePathOptions.pruneEnabled)
    {
        auto pruneExpr = pruneExpression(spec.namePathOptions);
        if (!pruneExpr.empty())
        {
            args.emplace_back("(");
            args.insert(args.end(), pruneExpr.begin(), pruneExpr.end());
            args.emplace_back("-prune");
            args.emplace_back("-o");
            if (!tests.empty())
                args.emplace_back("(");
            args.insert(args.end(), tests.begin(), tests.end());
            if (!tests.empty())
                args.emplace_back(")");
            if (includeActions && spec.enableActionOptions)
            {
                auto actions = buildActionTokens(spec.actionOptions);
                args.insert(args.end(), actions.begin(), actions.end());
            }
            else
            {
                args.emplace_back("-print");
            }
            args.emplace_back(")");
            return args;
        }
    }

    args.insert(args.end(), tests.begin(), tests.end());

    if (includeActions && spec.enableActionOptions)
    {
        ActionOptions actions = spec.actionOptions;
        ensurePrintAction(actions);
        auto actionTokens = buildActionTokens(actions);
        args.insert(args.end(), actionTokens.begin(), actionTokens.end());
    }
    else
    {
        args.emplace_back("-print");
    }

    return args;
}

SearchExecutionResult executeSpecification(const SearchSpecification &spec,
                                           const SearchExecutionOptions &options,
                                           std::ostream *forwardStdout,
                                           std::ostream *forwardStderr)
{
    SearchSpecification execSpec = spec;
    if (!options.includeActions)
        execSpec.enableActionOptions = false;
    auto command = buildFindCommand(execSpec, options.includeActions);
    auto result = executeCommand(command, options.captureMatches, forwardStdout, forwardStderr);

    if (options.captureMatches && options.filterContent && spec.enableTextSearch && spec.textOptions.searchInContents)
    {
        std::vector<std::filesystem::path> filtered;
        std::vector<std::string> terms = spec.textOptions.allowMultipleTerms
                                             ? tokenizeTerms(trimCopy(arrayToString(spec.searchText)))
                                             : std::vector<std::string>{trimCopy(arrayToString(spec.searchText))};
        std::string pattern = arrayToString(spec.searchText);
        for (const auto &match : result.matches)
        {
            if (!std::filesystem::is_regular_file(match))
            {
                filtered.push_back(match);
                continue;
            }
            if (fileMatchesContent(match,
                                   spec.textOptions,
                                   terms,
                                   pattern))
            {
                filtered.push_back(match);
            }
        }
        result.matches = std::move(filtered);
    }

    if (options.captureMatches && forwardStdout)
    {
        for (const auto &match : result.matches)
            (*forwardStdout) << match.string() << '\n';
        forwardStdout->flush();
    }

    return result;
}

} // namespace ck::find
