// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage_info.hpp"
namespace ck::files {
enum class InodeFlagQueryFamily { Unknown, LinuxIoctlGetFlags };
struct FileInodeFlags {
    InodeFlagQueryFamily family = InodeFlagQueryFamily::Unknown;
    Observation<ObjectIdentity> owner;
    ObjectKind kind = ObjectKind::Other;
    std::uint32_t raw = 0;
};
enum class LinuxInodeFlag {
    Compression, Synchronous, Immutable, AppendOnly, NoDump, NoAtime,
    NoCompression, Encryption, JournalData, DirectorySync, Verity, NoCopyOnWrite,
    Dax, ProjectInherit, Casefold
};
// Literal FS_IOC_GETFLAGS bit only, NOT statx/BSD/xflags. Unknown raw bits are
// retained. Clear is not feature absence; Set is not effective behavior,
// allocated representation, preservation/replay support or write authority.
Observation<ReportedFlag> reported_inode_flag(const FileInodeFlags& info, LinuxInodeFlag flag);
namespace detail {
Observation<ObjectIdentity> qualify_inode_flags_subject(const Observation<BasicMetadata>& before, const Observation<BasicMetadata>& after);
}
} // namespace ck::files
