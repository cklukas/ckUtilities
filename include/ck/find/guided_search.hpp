#pragma once

#include "ck/find/search_model.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ck::find
{

enum class GuidedTypePreset : unsigned short
{
    All = 0,
    Documents,
    Images,
    Audio,
    Archives,
    Code,
    Custom
};

enum class GuidedDatePreset : unsigned short
{
    AnyTime = 0,
    PastDay,
    PastWeek,
    PastMonth,
    PastSixMonths,
    PastYear,
    CustomRange
};

enum class GuidedSizePreset : unsigned short
{
    AnySize = 0,
    LargerThan,
    SmallerThan,
    Between,
    Exactly,
    EmptyOnly
};

struct GuidedSearchState
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

    bool searchFileContents = true;
    bool searchFileNames = true;
    TextSearchOptions::Mode textMode = TextSearchOptions::Mode::Contains;
    bool textMatchCase = false;
    bool textAllowMultipleTerms = false;
    bool textTreatBinaryAsText = false;

    GuidedTypePreset typePreset = GuidedTypePreset::All;
    bool typeCustomCaseSensitive = false;
    std::array<char, 256> typeCustomExtensions{};
    std::array<char, 256> typeCustomDetectors{};

    GuidedDatePreset datePreset = GuidedDatePreset::AnyTime;
    std::array<char, 32> dateFrom{};
    std::array<char, 32> dateTo{};

    GuidedSizePreset sizePreset = GuidedSizePreset::AnySize;
    std::array<char, 32> sizePrimary{};
    std::array<char, 32> sizeSecondary{};
    bool sizeUseDecimalUnits = false;

    bool includePermissionAudit = false;
    bool includeTraversalFineTune = false;
    bool includeActionTweaks = true;

    bool previewResults = true;
    bool listMatches = true;
    bool deleteMatches = false;
    bool runCommand = false;

    std::array<char, 256> customCommand{};

    bool operator==(const GuidedSearchState &) const = default;
};

struct GuidedSearchPreset
{
    std::string_view id;
    std::string_view title;
    std::string_view subtitle;
    void (*apply)(GuidedSearchState &state);
};

struct GuidedRecipe
{
    std::string_view id;
    std::string_view title;
    std::string_view description;
    void (*apply)(GuidedSearchState &state);
};

GuidedSearchState guidedStateFromSpecification(const SearchSpecification &spec);
void applyGuidedStateToSpecification(const GuidedSearchState &state, SearchSpecification &spec);

std::span<const GuidedSearchPreset> popularSearchPresets();
std::span<const GuidedRecipe> expertSearchRecipes();

GuidedTypePreset detectTypePreset(const TypeFilterOptions &options);

} // namespace ck::find
