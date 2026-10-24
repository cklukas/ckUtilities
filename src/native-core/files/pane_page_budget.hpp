// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "file_metadata.hpp"
namespace ck::files::detail {
inline bool charge(std::size_t bytes, std::size_t &used, std::size_t limit) {
    if (bytes > limit - used)
        return false;
    used += bytes;
    return true;
}
template <class T> inline bool charge(const Observation<T> &value, std::size_t &used, std::size_t limit) {
    const auto *detail = value.unavailable_detail();
    return !detail || charge(detail->error.operation.size(), used, limit);
}
inline bool charge(const BasicMetadata &value, std::size_t &used, std::size_t limit) {
    return charge(value.identity, used, limit) && charge(value.kind, used, limit) &&
           charge(value.logical_size, used, limit) && charge(value.allocated_size, used, limit) &&
           charge(value.link_count, used, limit) && charge(value.posix_uid, used, limit) &&
           charge(value.posix_gid, used, limit) && charge(value.posix_permissions, used, limit) &&
           charge(value.bsd_flags, used, limit) && charge(value.windows_attributes, used, limit) &&
           charge(value.reparse_tag, used, limit) && charge(value.linux_attributes, used, limit) &&
           charge(value.mount_id, used, limit) && charge(value.accessed, used, limit) &&
           charge(value.modified, used, limit) && charge(value.changed, used, limit) &&
           charge(value.created, used, limit);
}
inline bool charge(const std::optional<UnavailableObservation> &value, std::size_t &used, std::size_t limit) {
    return !value || charge(value->error.operation.size(), used, limit);
}
} // namespace ck::files::detail
