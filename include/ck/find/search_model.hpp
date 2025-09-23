#pragma once

#include <array>
#include <limits.h>

namespace ck::find
{

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct TextSearchOptions
{
    enum class Mode : unsigned short
    {
        Contains = 0,
        WholeWord = 1,
        RegularExpression = 2
    };

    Mode mode = Mode::Contains;
    bool matchCase = false;
    bool searchInContents = true;
    bool searchInFileNames = true;
    bool allowMultipleTerms = false;
    bool treatBinaryAsText = false;
};

struct NamePathOptions
{
    enum class PruneTest : unsigned short
    {
        Name = 0,
        Iname,
        Path,
        Ipath,
        Regex,
        Iregex
    };

    bool nameEnabled = false;
    bool inameEnabled = false;
    bool pathEnabled = false;
    bool ipathEnabled = false;
    bool regexEnabled = false;
    bool iregexEnabled = false;
    bool lnameEnabled = false;
    bool ilnameEnabled = false;
    bool pruneEnabled = false;
    bool pruneDirectoriesOnly = true;
    PruneTest pruneTest = PruneTest::Path;
    std::array<char, 256> namePattern{};
    std::array<char, 256> inamePattern{};
    std::array<char, 256> pathPattern{};
    std::array<char, 256> ipathPattern{};
    std::array<char, 256> regexPattern{};
    std::array<char, 256> iregexPattern{};
    std::array<char, 256> lnamePattern{};
    std::array<char, 256> ilnamePattern{};
    std::array<char, 256> prunePattern{};
};

struct TimeFilterOptions
{
    enum class Preset : unsigned short
    {
        AnyTime = 0,
        PastDay,
        PastWeek,
        PastMonth,
        PastSixMonths,
        PastYear,
        PastSixYears,
        CustomRange
    };

    Preset preset = Preset::AnyTime;
    bool includeModified = true;
    bool includeCreated = false;
    bool includeAccessed = false;
    std::array<char, 32> customFrom{};
    std::array<char, 32> customTo{};

    bool useMTime = false;
    bool useATime = false;
    bool useCTime = false;
    bool useMMin = false;
    bool useAMin = false;
    bool useCMin = false;
    bool useUsed = false;
    bool useNewer = false;
    bool useANewer = false;
    bool useCNewer = false;
    bool useNewermt = false;
    bool useNewerat = false;
    bool useNewerct = false;
    std::array<char, 16> mtime{};
    std::array<char, 16> atime{};
    std::array<char, 16> ctime{};
    std::array<char, 16> mmin{};
    std::array<char, 16> amin{};
    std::array<char, 16> cmin{};
    std::array<char, 16> used{};
    std::array<char, PATH_MAX> newer{};
    std::array<char, PATH_MAX> anewer{};
    std::array<char, PATH_MAX> cnewer{};
    std::array<char, 64> newermt{};
    std::array<char, 64> newerat{};
    std::array<char, 64> newerct{};
};

struct SizeFilterOptions
{
    bool minEnabled = false;
    bool maxEnabled = false;
    bool exactEnabled = false;
    bool rangeInclusive = true;
    bool includeZeroByte = true;
    bool treatDirectoriesAsFiles = false;
    bool useDecimalUnits = false;
    bool emptyEnabled = false;
    std::array<char, 32> minSpec{};
    std::array<char, 32> maxSpec{};
    std::array<char, 32> exactSpec{};
};

struct TypeFilterOptions
{
    bool typeEnabled = false;
    bool xtypeEnabled = false;
    bool useExtensions = false;
    bool extensionCaseInsensitive = true;
    bool useDetectors = false;
    std::array<char, 16> typeLetters{};
    std::array<char, 16> xtypeLetters{};
    std::array<char, 256> extensions{};
    std::array<char, 256> detectorTags{};
};

struct PermissionOwnershipOptions
{
    enum class PermMode : unsigned short
    {
        Exact = 0,
        AllBits,
        AnyBit
    };

    bool permEnabled = false;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    PermMode permMode = PermMode::Exact;
    std::array<char, 16> permSpec{};

    bool userEnabled = false;
    bool uidEnabled = false;
    bool groupEnabled = false;
    bool gidEnabled = false;
    bool noUser = false;
    bool noGroup = false;
    std::array<char, 64> user{};
    std::array<char, 32> uid{};
    std::array<char, 64> group{};
    std::array<char, 32> gid{};
};

struct TraversalFilesystemOptions
{
    enum class SymlinkMode : unsigned short
    {
        Physical = 0,
        CommandLine,
        Everywhere
    };

    enum class WarningMode : unsigned short
    {
        Default = 0,
        ForceWarn,
        SuppressWarn
    };

    SymlinkMode symlinkMode = SymlinkMode::Physical;
    WarningMode warningMode = WarningMode::Default;
    bool depthFirst = false;
    bool stayOnFilesystem = false;
    bool assumeNoLeaf = false;
    bool ignoreReaddirRace = false;
    bool dayStart = false;
    bool maxDepthEnabled = false;
    bool minDepthEnabled = false;
    bool filesFromEnabled = false;
    bool filesFromNullSeparated = false;
    bool fstypeEnabled = false;
    bool linksEnabled = false;
    bool sameFileEnabled = false;
    bool inumEnabled = false;
    std::array<char, 8> maxDepth{};
    std::array<char, 8> minDepth{};
    std::array<char, PATH_MAX> filesFrom{};
    std::array<char, 64> fsType{};
    std::array<char, 16> linkCount{};
    std::array<char, PATH_MAX> sameFile{};
    std::array<char, 32> inode{};
};

struct ActionOptions
{
    enum class ExecVariant : unsigned short
    {
        Exec = 0,
        ExecDir,
        Ok,
        OkDir
    };

    bool print = true;
    bool print0 = false;
    bool ls = false;
    bool deleteMatches = false;
    bool quitEarly = false;
    bool execEnabled = false;
    bool execUsePlus = false;
    ExecVariant execVariant = ExecVariant::Exec;
    bool fprintEnabled = false;
    bool fprintAppend = false;
    bool fprint0Enabled = false;
    bool fprint0Append = false;
    bool flsEnabled = false;
    bool flsAppend = false;
    bool printfEnabled = false;
    bool fprintfEnabled = false;
    bool fprintfAppend = false;
    std::array<char, 512> execCommand{};
    std::array<char, PATH_MAX> fprintFile{};
    std::array<char, PATH_MAX> fprint0File{};
    std::array<char, PATH_MAX> flsFile{};
    std::array<char, 256> printfFormat{};
    std::array<char, PATH_MAX> fprintfFile{};
    std::array<char, 256> fprintfFormat{};
};

struct SearchSpecification
{
    std::array<char, 128> specName{};
    std::array<char, PATH_MAX> startLocation{};
    std::array<char, 256> searchText{};
    std::array<char, 256> includePatterns{};
    std::array<char, 256> excludePatterns{};
    bool includeSubdirectories = true;
    bool includeHidden = false;
    bool followSymlinks = false;
    bool stayOnSameFilesystem = false;
    bool enableTextSearch = true;
    bool enableNamePathTests = false;
    bool enableTimeFilters = false;
    bool enableSizeFilters = false;
    bool enableTypeFilters = false;
    bool enablePermissionOwnership = false;
    bool enableTraversalFilters = false;
    bool enableActionOptions = true;
    TextSearchOptions textOptions{};
    NamePathOptions namePathOptions{};
    TimeFilterOptions timeOptions{};
    SizeFilterOptions sizeOptions{};
    TypeFilterOptions typeOptions{};
    PermissionOwnershipOptions permissionOptions{};
    TraversalFilesystemOptions traversalOptions{};
    ActionOptions actionOptions{};
};

SearchSpecification makeDefaultSpecification();

} // namespace ck::find

