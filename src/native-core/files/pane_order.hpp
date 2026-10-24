// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_name.hpp"
#include "file_metadata.hpp"
namespace ck::files {
enum class PaneSortKey { Name, Kind, Size, Modified };
struct PaneOrder {
    PaneSortKey key = PaneSortKey::Name;
    bool descending = false;
    bool folders_first = true;
};
bool valid_pane_order(PaneOrder order) noexcept;
// Shared deterministic worker ordering. Null metadata and unavailable fields
// remain after available keys in either direction. Name ties are ascending;
// callers may add a stable original-row-ID tie for duplicate native names.
// No display parsing, locale, filesystem access or link following.
int compare_pane_entries(const NativeName &a, const BasicMetadata *am, const NativeName &b, const BasicMetadata *bm,
                         PaneOrder order);
} // namespace ck::files
