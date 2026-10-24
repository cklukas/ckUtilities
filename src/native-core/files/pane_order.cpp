// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_order.hpp"
#include <functional>
#include <stdexcept>
namespace ck::files {
namespace {
template <class T, class Less>
int observed(const Observation<T> *a, const Observation<T> *b, bool descending, Less less) {
    const auto *av = a ? a->value() : nullptr;
    const auto *bv = b ? b->value() : nullptr;
    if (!av || !bv)
        return av ? -1 : bv ? 1 : 0;
    const int result = less(*av, *bv) ? -1 : less(*bv, *av) ? 1 : 0;
    return descending ? -result : result;
}
bool directory(const BasicMetadata *m) {
    return m && m->kind.value() && *m->kind.value() == ObjectKind::Directory;
}
} // namespace
bool valid_pane_order(PaneOrder order) noexcept {
    switch (order.key) {
    case PaneSortKey::Name:
    case PaneSortKey::Kind:
    case PaneSortKey::Size:
    case PaneSortKey::Modified:
        return true;
    }
    return false;
}
int compare_pane_entries(const NativeName &a, const BasicMetadata *am, const NativeName &b, const BasicMetadata *bm,
                         PaneOrder order) {
    if (!valid_pane_order(order))
        throw std::invalid_argument("invalid pane order");
    if (order.folders_first && directory(am) != directory(bm))
        return directory(am) ? -1 : 1;
    int result = 0;
    switch (order.key) {
    case PaneSortKey::Name:
        result = compare_native_names(a, b);
        if (order.descending)
            result = -result;
        break;
    case PaneSortKey::Kind:
        result =
            observed(am ? &am->kind : nullptr, bm ? &bm->kind : nullptr, order.descending, std::less<ObjectKind>{});
        break;
    case PaneSortKey::Size:
        result = observed(am ? &am->logical_size : nullptr, bm ? &bm->logical_size : nullptr, order.descending,
                          std::less<std::uint64_t>{});
        break;
    case PaneSortKey::Modified:
        result = observed(am ? &am->modified : nullptr, bm ? &bm->modified : nullptr, order.descending,
                          [](const FileTimestamp &x, const FileTimestamp &y) {
                              return x.seconds() < y.seconds() ||
                                     (x.seconds() == y.seconds() && x.nanoseconds() < y.nanoseconds());
                          });
        break;
    }
    return result ? result : compare_native_names(a, b);
}
} // namespace ck::files
