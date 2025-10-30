#pragma once

#include "ck/commands/ck_find.hpp"
#include "ck/commands/common.hpp"

enum CommandId : unsigned short
{
    cmNewSearch = ck::commands::find::NewSearch,
    cmLoadSpec = ck::commands::find::LoadSpec,
    cmSaveSpec = ck::commands::find::SaveSpec,
    cmReturnToLauncher = ck::commands::find::ReturnToLauncher,
    cmAbout = ck::commands::find::About,
    cmBrowseStart = ck::commands::find::BrowseStart,
    cmTextOptions = ck::commands::find::TextOptions,
    cmNamePathOptions = ck::commands::find::NamePathOptions,
    cmTimeFilters = ck::commands::find::TimeFilters,
    cmSizeFilters = ck::commands::find::SizeFilters,
    cmTypeFilters = ck::commands::find::TypeFilters,
    cmPermissionOwnership = ck::commands::find::PermissionOwnership,
    cmTraversalFilters = ck::commands::find::TraversalFilters,
    cmActionOptions = ck::commands::find::ActionOptions,
    cmDialogLoadSpec = ck::commands::find::DialogLoadSpec,
    cmDialogSaveSpec = ck::commands::find::DialogSaveSpec,
    cmTogglePreview = ck::commands::find::TogglePreview,
    cmTabQuickStart = ck::commands::find::TabQuickStart,
    cmTabContentNames = ck::commands::find::TabContentNames,
    cmTabDatesSizes = ck::commands::find::TabDatesSizes,
    cmTabTypesOwnership = ck::commands::find::TabTypesOwnership,
    cmTabTraversal = ck::commands::find::TabTraversal,
    cmTabActions = ck::commands::find::TabActions,
    cmTabNext = ck::commands::common::TabNext,
    cmTabPrevious = ck::commands::common::TabPrevious,
};
