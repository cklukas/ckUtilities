#include "ck/find/guided_search.hpp"
#include "ck/find/search_model.hpp"

#include <gtest/gtest.h>

using namespace ck::find;

namespace
{

const GuidedSearchPreset *presetById(std::string_view id)
{
    for (const auto &preset : popularSearchPresets())
    {
        if (preset.id == id)
            return &preset;
    }
    return nullptr;
}

const GuidedRecipe *recipeById(std::string_view id)
{
    for (const auto &recipe : expertSearchRecipes())
    {
        if (recipe.id == id)
            return &recipe;
    }
    return nullptr;
}

} // namespace

TEST(GuidedSearchPresets, RecentDocumentsPresetSetsDocumentFilters)
{
    SearchSpecification spec = makeDefaultSpecification();
    GuidedSearchState state = guidedStateFromSpecification(spec);

    const auto *preset = presetById("recent-documents");
    ASSERT_NE(nullptr, preset);
    ASSERT_NE(nullptr, preset->apply);

    preset->apply(state);
    applyGuidedStateToSpecification(state, spec);

    EXPECT_TRUE(spec.enableTypeFilters);
    EXPECT_STREQ(bufferToString(spec.typeOptions.extensions).c_str(), "pdf,doc,docx,txt,md,rtf");
    EXPECT_TRUE(spec.enableTimeFilters);
    EXPECT_EQ(spec.timeOptions.preset, TimeFilterOptions::Preset::PastWeek);
    EXPECT_TRUE(spec.textOptions.searchInContents);
    EXPECT_TRUE(spec.textOptions.searchInFileNames);
}

TEST(GuidedSearchPresets, LargeVideosPresetConfiguresSizeAndTypes)
{
    SearchSpecification spec = makeDefaultSpecification();
    GuidedSearchState state = guidedStateFromSpecification(spec);

    const auto *preset = presetById("large-videos");
    ASSERT_NE(nullptr, preset);
    ASSERT_NE(nullptr, preset->apply);

    preset->apply(state);
    applyGuidedStateToSpecification(state, spec);

    EXPECT_TRUE(spec.enableTypeFilters);
    EXPECT_STREQ(bufferToString(spec.typeOptions.extensions).c_str(), "mp4,mkv,mov,avi,webm");
    EXPECT_TRUE(spec.enableSizeFilters);
    EXPECT_TRUE(spec.sizeOptions.minEnabled);
    EXPECT_STREQ(bufferToString(spec.sizeOptions.minSpec).c_str(), "500M");
    EXPECT_FALSE(spec.sizeOptions.maxEnabled);
}

TEST(GuidedSearchRoundTrip, GuidedStateAndSpecificationRemainConsistent)
{
    SearchSpecification spec = makeDefaultSpecification();
    std::snprintf(spec.startLocation.data(), spec.startLocation.size(), "/tmp");
    spec.enableTypeFilters = true;
    spec.typeOptions.useExtensions = true;
    copyToArray(spec.typeOptions.extensions, "png,jpg");
    spec.enableSizeFilters = true;
    spec.sizeOptions.minEnabled = true;
    copyToArray(spec.sizeOptions.minSpec, "2M");
    spec.sizeOptions.maxEnabled = true;
    copyToArray(spec.sizeOptions.maxSpec, "20M");
    spec.enableTimeFilters = true;
    spec.timeOptions.preset = TimeFilterOptions::Preset::PastMonth;

    GuidedSearchState state = guidedStateFromSpecification(spec);
    EXPECT_EQ(GuidedTypePreset::Custom, state.typePreset);
    EXPECT_STREQ(state.typeCustomExtensions.data(), "png,jpg");
    EXPECT_EQ(GuidedSizePreset::Between, state.sizePreset);
    EXPECT_STREQ(state.sizePrimary.data(), "2M");
    EXPECT_STREQ(state.sizeSecondary.data(), "20M");
    EXPECT_EQ(GuidedDatePreset::PastMonth, state.datePreset);

    state.sizePreset = GuidedSizePreset::Exactly;
    std::snprintf(state.sizePrimary.data(), state.sizePrimary.size(), "10M");
    applyGuidedStateToSpecification(state, spec);

    EXPECT_TRUE(spec.sizeOptions.exactEnabled);
    EXPECT_STREQ(bufferToString(spec.sizeOptions.exactSpec).c_str(), "10M");
    EXPECT_FALSE(spec.sizeOptions.minEnabled);
    EXPECT_FALSE(spec.sizeOptions.maxEnabled);
}

TEST(GuidedSearchRecipes, OwnedRootRecipeEnablesPermissionAudit)
{
    SearchSpecification spec = makeDefaultSpecification();
    GuidedSearchState state = guidedStateFromSpecification(spec);

    const auto *recipe = recipeById("owned-root");
    ASSERT_NE(nullptr, recipe);
    ASSERT_NE(nullptr, recipe->apply);

    recipe->apply(state);
    EXPECT_TRUE(state.includePermissionAudit);
}
