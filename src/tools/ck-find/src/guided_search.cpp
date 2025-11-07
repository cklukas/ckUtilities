#include "ck/find/guided_search.hpp"

#include "ck/find/cli_buffer_utils.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ck::find
{

namespace
{

void resetCommonDefaults(GuidedSearchState &state)
{
    state.searchFileContents = true;
    state.searchFileNames = true;
    state.textMode = TextSearchOptions::Mode::Contains;
    state.textMatchCase = false;
    state.textAllowMultipleTerms = false;
    state.textTreatBinaryAsText = false;
    state.typePreset = GuidedTypePreset::All;
    state.typeCustomExtensions[0] = '\0';
    state.typeCustomDetectors[0] = '\0';
    state.typeCustomCaseSensitive = false;
    state.datePreset = GuidedDatePreset::AnyTime;
    state.dateFrom[0] = '\0';
    state.dateTo[0] = '\0';
    state.sizePreset = GuidedSizePreset::AnySize;
    state.sizePrimary[0] = '\0';
    state.sizeSecondary[0] = '\0';
    state.includePermissionAudit = false;
    state.includeTraversalFineTune = false;
    state.includeActionTweaks = true;
    state.previewResults = true;
    state.listMatches = true;
    state.deleteMatches = false;
    state.runCommand = false;
    state.customCommand[0] = '\0';
}

void applyRecentDocuments(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.typePreset = GuidedTypePreset::Documents;
    std::snprintf(state.typeCustomExtensions.data(), state.typeCustomExtensions.size(), "pdf,doc,docx,txt,md,rtf");
    state.datePreset = GuidedDatePreset::PastWeek;
}

void applyLargeVideos(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.typePreset = GuidedTypePreset::Custom;
    std::snprintf(state.typeCustomExtensions.data(), state.typeCustomExtensions.size(), "mp4,mkv,mov,avi,webm");
    state.sizePreset = GuidedSizePreset::LargerThan;
    std::snprintf(state.sizePrimary.data(), state.sizePrimary.size(), "500M");
    state.searchFileContents = false;
    state.searchFileNames = true;
}

void applyDuplicatesByName(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.searchFileContents = false;
    state.searchFileNames = true;
    state.typePreset = GuidedTypePreset::All;
    state.previewResults = true;
    state.listMatches = true;
}

void applyStaleArchives(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.typePreset = GuidedTypePreset::Archives;
    std::snprintf(state.typeCustomExtensions.data(), state.typeCustomExtensions.size(), "zip,tar,tar.gz,tgz,rar,7z");
    state.datePreset = GuidedDatePreset::PastSixMonths;
}

void applyFreshCodeChanges(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.typePreset = GuidedTypePreset::Code;
    std::snprintf(state.typeCustomExtensions.data(), state.typeCustomExtensions.size(), "c,cpp,h,hpp,cc,hh,py,js,ts,java,rb,rs,go,swift,cs");
    state.datePreset = GuidedDatePreset::PastDay;
}

void applyDeployWeekRecipe(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.typePreset = GuidedTypePreset::Code;
    std::snprintf(state.typeCustomExtensions.data(), state.typeCustomExtensions.size(), "c,cpp,h,hpp,cc,hh,py,js,ts,java,rb,rs,go,swift,cs");
    state.datePreset = GuidedDatePreset::PastWeek;
}

void applyOwnedRootRecipe(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.includePermissionAudit = true;
}

void applyNewSymlinksRecipe(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.includeTraversalFineTune = true;
    state.datePreset = GuidedDatePreset::PastWeek;
}

void applyEmptyDirsRecipe(GuidedSearchState &state)
{
    resetCommonDefaults(state);
    state.sizePreset = GuidedSizePreset::EmptyOnly;
    state.includeTraversalFineTune = true;
}

constexpr std::array<GuidedSearchPreset, 5> kPopularPresets{{
    {"recent-documents", "Recent documents", "Documents touched in the last 7 days", applyRecentDocuments},
    {"large-videos", "Large videos", "Video files bigger than 500 MiB", applyLargeVideos},
    {"duplicates-by-name", "Duplicates by name", "Surface files grouped by name for manual review", applyDuplicatesByName},
    {"stale-archives", "Stale archives", "Archives older than six months", applyStaleArchives},
    {"fresh-code", "Fresh code changes", "Source files edited in the last 24 hours", applyFreshCodeChanges},
}};

constexpr std::array<GuidedRecipe, 4> kExpertRecipes{{
    {"deploy-week", "Changed in last deploy", "Project files updated in the past 7 days", applyDeployWeekRecipe},
    {"owned-root", "Root-owned & group writable", "Audit permission issues under /srv/www", applyOwnedRootRecipe},
    {"new-symlinks", "New symlinks outside project", "Detect symlinks created this week", applyNewSymlinksRecipe},
    {"empty-dirs", "Empty directories cleanup", "Find empty directories ready for removal", applyEmptyDirsRecipe},
}};

} // namespace

GuidedSearchState guidedStateFromSpecification(const SearchSpecification &spec)
{
    GuidedSearchState state{};
    copyToArray(state.specName, spec.specName);
    copyToArray(state.startLocation, spec.startLocation);
    copyToArray(state.searchText, spec.searchText);
    copyToArray(state.includePatterns, spec.includePatterns);
    copyToArray(state.excludePatterns, spec.excludePatterns);

    state.includeSubdirectories = spec.includeSubdirectories;
    state.includeHidden = spec.includeHidden;
    state.followSymlinks = spec.followSymlinks;
    state.stayOnSameFilesystem = spec.stayOnSameFilesystem || spec.traversalOptions.stayOnFilesystem;

    state.searchFileContents = spec.textOptions.searchInContents;
    state.searchFileNames = spec.textOptions.searchInFileNames;
    state.textMode = spec.textOptions.mode;
    state.textMatchCase = spec.textOptions.matchCase;
    state.textAllowMultipleTerms = spec.textOptions.allowMultipleTerms;
    state.textTreatBinaryAsText = spec.textOptions.treatBinaryAsText;

    state.typePreset = detectTypePreset(spec.typeOptions);
    copyToArray(state.typeCustomExtensions, spec.typeOptions.extensions);
    state.typeCustomCaseSensitive = !spec.typeOptions.extensionCaseInsensitive;
    copyToArray(state.typeCustomDetectors, spec.typeOptions.detectorTags);

    switch (spec.timeOptions.preset)
    {
    case TimeFilterOptions::Preset::PastDay:
        state.datePreset = GuidedDatePreset::PastDay;
        break;
    case TimeFilterOptions::Preset::PastWeek:
        state.datePreset = GuidedDatePreset::PastWeek;
        break;
    case TimeFilterOptions::Preset::PastMonth:
        state.datePreset = GuidedDatePreset::PastMonth;
        break;
    case TimeFilterOptions::Preset::PastSixMonths:
        state.datePreset = GuidedDatePreset::PastSixMonths;
        break;
    case TimeFilterOptions::Preset::PastYear:
        state.datePreset = GuidedDatePreset::PastYear;
        break;
    case TimeFilterOptions::Preset::CustomRange:
        state.datePreset = GuidedDatePreset::CustomRange;
        copyToArray(state.dateFrom, spec.timeOptions.customFrom);
        copyToArray(state.dateTo, spec.timeOptions.customTo);
        break;
    case TimeFilterOptions::Preset::PastSixYears:
    case TimeFilterOptions::Preset::AnyTime:
    default:
        state.datePreset = GuidedDatePreset::AnyTime;
        break;
    }

    if (spec.sizeOptions.exactEnabled)
    {
        state.sizePreset = GuidedSizePreset::Exactly;
        copyToArray(state.sizePrimary, spec.sizeOptions.exactSpec);
    }
    else if (spec.sizeOptions.minEnabled && spec.sizeOptions.maxEnabled)
    {
        state.sizePreset = GuidedSizePreset::Between;
        copyToArray(state.sizePrimary, spec.sizeOptions.minSpec);
        copyToArray(state.sizeSecondary, spec.sizeOptions.maxSpec);
    }
    else if (spec.sizeOptions.minEnabled)
    {
        state.sizePreset = GuidedSizePreset::LargerThan;
        copyToArray(state.sizePrimary, spec.sizeOptions.minSpec);
    }
    else if (spec.sizeOptions.maxEnabled)
    {
        state.sizePreset = GuidedSizePreset::SmallerThan;
        copyToArray(state.sizePrimary, spec.sizeOptions.maxSpec);
    }
    else if (spec.sizeOptions.emptyEnabled)
    {
        state.sizePreset = GuidedSizePreset::EmptyOnly;
    }
    else
    {
        state.sizePreset = GuidedSizePreset::AnySize;
    }
    state.sizeUseDecimalUnits = spec.sizeOptions.useDecimalUnits;

    state.includePermissionAudit = spec.enablePermissionOwnership;
    state.includeTraversalFineTune = spec.enableTraversalFilters;
    state.includeActionTweaks = spec.enableActionOptions;

    state.previewResults = true;
    state.listMatches = spec.actionOptions.print;
    state.deleteMatches = spec.actionOptions.deleteMatches;
    state.runCommand = spec.actionOptions.execEnabled;
    copyToArray(state.customCommand, spec.actionOptions.execCommand);

    return state;
}

void applyGuidedStateToSpecification(const GuidedSearchState &state, SearchSpecification &spec)
{
    copyToArray(spec.specName, state.specName);
    copyToArray(spec.startLocation, state.startLocation);
    copyToArray(spec.searchText, state.searchText);
    copyToArray(spec.includePatterns, state.includePatterns);
    copyToArray(spec.excludePatterns, state.excludePatterns);

    spec.includeSubdirectories = state.includeSubdirectories;
    spec.includeHidden = state.includeHidden;
    spec.followSymlinks = state.followSymlinks;
    spec.stayOnSameFilesystem = state.stayOnSameFilesystem;
    spec.traversalOptions.stayOnFilesystem = state.stayOnSameFilesystem;

    spec.enableTextSearch = state.searchFileContents || state.searchFileNames;
    spec.textOptions.searchInContents = state.searchFileContents;
    spec.textOptions.searchInFileNames = state.searchFileNames;
    spec.textOptions.mode = state.textMode;
    spec.textOptions.matchCase = state.textMatchCase;
    spec.textOptions.allowMultipleTerms = state.textAllowMultipleTerms;
    spec.textOptions.treatBinaryAsText = state.textTreatBinaryAsText;

    spec.enableTypeFilters = (state.typePreset != GuidedTypePreset::All);
    spec.typeOptions.useExtensions = spec.enableTypeFilters;
    spec.typeOptions.extensionCaseInsensitive = !state.typeCustomCaseSensitive;
    copyToArray(spec.typeOptions.extensions, state.typeCustomExtensions);
    copyToArray(spec.typeOptions.detectorTags, state.typeCustomDetectors);

    spec.enableTimeFilters = (state.datePreset != GuidedDatePreset::AnyTime);
    spec.timeOptions.includeModified = true;
    spec.timeOptions.includeAccessed = false;
    spec.timeOptions.includeCreated = false;
    switch (state.datePreset)
    {
    case GuidedDatePreset::PastDay:
        spec.timeOptions.preset = TimeFilterOptions::Preset::PastDay;
        break;
    case GuidedDatePreset::PastWeek:
        spec.timeOptions.preset = TimeFilterOptions::Preset::PastWeek;
        break;
    case GuidedDatePreset::PastMonth:
        spec.timeOptions.preset = TimeFilterOptions::Preset::PastMonth;
        break;
    case GuidedDatePreset::PastSixMonths:
        spec.timeOptions.preset = TimeFilterOptions::Preset::PastSixMonths;
        break;
    case GuidedDatePreset::PastYear:
        spec.timeOptions.preset = TimeFilterOptions::Preset::PastYear;
        break;
    case GuidedDatePreset::CustomRange:
        spec.timeOptions.preset = TimeFilterOptions::Preset::CustomRange;
        copyToArray(spec.timeOptions.customFrom, state.dateFrom);
        copyToArray(spec.timeOptions.customTo, state.dateTo);
        break;
    case GuidedDatePreset::AnyTime:
    default:
        spec.timeOptions.preset = TimeFilterOptions::Preset::AnyTime;
        break;
    }

    spec.enableSizeFilters = (state.sizePreset != GuidedSizePreset::AnySize);
    spec.sizeOptions.minEnabled = false;
    spec.sizeOptions.maxEnabled = false;
    spec.sizeOptions.exactEnabled = false;
    spec.sizeOptions.emptyEnabled = false;
    spec.sizeOptions.useDecimalUnits = state.sizeUseDecimalUnits;
    switch (state.sizePreset)
    {
    case GuidedSizePreset::LargerThan:
        spec.sizeOptions.minEnabled = true;
        copyToArray(spec.sizeOptions.minSpec, state.sizePrimary);
        break;
    case GuidedSizePreset::SmallerThan:
        spec.sizeOptions.maxEnabled = true;
        copyToArray(spec.sizeOptions.maxSpec, state.sizePrimary);
        break;
    case GuidedSizePreset::Between:
        spec.sizeOptions.minEnabled = true;
        spec.sizeOptions.maxEnabled = true;
        copyToArray(spec.sizeOptions.minSpec, state.sizePrimary);
        copyToArray(spec.sizeOptions.maxSpec, state.sizeSecondary);
        break;
    case GuidedSizePreset::Exactly:
        spec.sizeOptions.exactEnabled = true;
        copyToArray(spec.sizeOptions.exactSpec, state.sizePrimary);
        break;
    case GuidedSizePreset::EmptyOnly:
        spec.sizeOptions.emptyEnabled = true;
        break;
    case GuidedSizePreset::AnySize:
    default:
        break;
    }

    spec.enablePermissionOwnership = state.includePermissionAudit;
    spec.enableTraversalFilters = state.includeTraversalFineTune;
    spec.enableActionOptions = state.includeActionTweaks || state.deleteMatches || state.runCommand || !state.listMatches;

    spec.actionOptions.print = state.listMatches;
    spec.actionOptions.deleteMatches = state.deleteMatches;
    spec.actionOptions.execEnabled = state.runCommand;
    spec.actionOptions.execUsePlus = false;
    spec.actionOptions.print0 = false;
    spec.actionOptions.ls = false;
    copyToArray(spec.actionOptions.execCommand, state.customCommand);
}

std::span<const GuidedSearchPreset> popularSearchPresets()
{
    return kPopularPresets;
}

std::span<const GuidedRecipe> expertSearchRecipes()
{
    return kExpertRecipes;
}

GuidedTypePreset detectTypePreset(const TypeFilterOptions &options)
{
    if (!options.useExtensions || options.extensions[0] == '\0')
        return GuidedTypePreset::All;

    std::string extensions = bufferToString(options.extensions);
    auto matches = [&](std::string_view list) {
        return extensions == list;
    };

    if (matches("pdf,doc,docx,txt,md,rtf"))
        return GuidedTypePreset::Documents;
    if (matches("jpg,jpeg,png,gif,svg,webp,bmp"))
        return GuidedTypePreset::Images;
    if (matches("mp3,wav,flac,aac,ogg"))
        return GuidedTypePreset::Audio;
    if (matches("zip,tar,tar.gz,tgz,rar,7z"))
        return GuidedTypePreset::Archives;
    if (matches("c,cpp,h,hpp,cc,hh,py,js,ts,java,rb,rs,go,swift,cs"))
        return GuidedTypePreset::Code;
    return GuidedTypePreset::Custom;
}

} // namespace ck::find
