// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "native_filesystem.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif

namespace ck::files {
namespace {

NativeError adapter_error(AdapterError code, std::string operation) {
    return {ErrorDomain::Adapter, static_cast<std::int64_t>(code), std::move(operation)};
}

ObservationState errno_state(int code) noexcept {
    if (code == EACCES || code == EPERM) return ObservationState::Denied;
    if (code == ENOENT || code == ENOTDIR) return ObservationState::Gone;
    if (code == ESTALE) return ObservationState::Changed;
    if (code == ENOSYS || code == ENOTSUP || code == EOPNOTSUPP) return ObservationState::Unsupported;
    return ObservationState::Failed;
}

template <class T>
Observation<T> not_applicable(const char* operation) {
    return Observation<T>::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::NotApplicable, operation));
}

Observation<FileTimestamp> timestamp(std::int64_t seconds, std::int64_t nanos, const char* operation) {
    if (nanos < 0 || nanos >= 1'000'000'000)
        return Observation<FileTimestamp>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, operation));
    return Observation<FileTimestamp>::available(*FileTimestamp::from_unix(seconds, static_cast<std::uint32_t>(nanos)));
}

ObjectKind kind(mode_t mode) noexcept {
    if (S_ISREG(mode)) return ObjectKind::RegularFile;
    if (S_ISDIR(mode)) return ObjectKind::Directory;
    if (S_ISLNK(mode)) return ObjectKind::SymbolicLink;
    if (S_ISFIFO(mode)) return ObjectKind::Fifo;
    if (S_ISSOCK(mode)) return ObjectKind::Socket;
    if (S_ISCHR(mode)) return ObjectKind::CharacterDevice;
    if (S_ISBLK(mode)) return ObjectKind::BlockDevice;
    return ObjectKind::Other;
}

void platform_fields(BasicMetadata& result) {
    result.windows_attributes = not_applicable<std::uint32_t>("windows_attributes");
    result.reparse_tag = not_applicable<std::uint32_t>("reparse_tag");
}

Observation<BasicMetadata> stat_metadata(const std::filesystem::path& path, const AdapterScope& scope) {
    struct stat native{};
    if (lstat(path.c_str(), &native) != 0) {
        const int code = errno;
        return Observation<BasicMetadata>::unavailable(errno_state(code), {ErrorDomain::Posix, code, "lstat"});
    }
    BasicMetadata result;
    platform_fields(result);
    std::optional<std::uint64_t> generation;
#if defined(__APPLE__)
    generation = native.st_gen;
    result.bsd_flags = Observation<std::uint32_t>::available(native.st_flags);
    result.created = timestamp(native.st_birthtimespec.tv_sec, native.st_birthtimespec.tv_nsec, "stat.birth_time");
    result.accessed = timestamp(native.st_atimespec.tv_sec, native.st_atimespec.tv_nsec, "stat.access_time");
    result.modified = timestamp(native.st_mtimespec.tv_sec, native.st_mtimespec.tv_nsec, "stat.modified_time");
    result.changed = timestamp(native.st_ctimespec.tv_sec, native.st_ctimespec.tv_nsec, "stat.changed_time");
    result.linux_attributes = not_applicable<LinuxFileAttributes>("linux_attributes");
#else
    result.bsd_flags = not_applicable<std::uint32_t>("bsd_flags");
    result.created = Observation<FileTimestamp>::unavailable(ObservationState::Unsupported,
        adapter_error(AdapterError::UnsupportedQuery, "lstat.birth_time"));
    result.accessed = timestamp(native.st_atim.tv_sec, native.st_atim.tv_nsec, "stat.access_time");
    result.modified = timestamp(native.st_mtim.tv_sec, native.st_mtim.tv_nsec, "stat.modified_time");
    result.changed = timestamp(native.st_ctim.tv_sec, native.st_ctim.tv_nsec, "stat.changed_time");
#endif
    result.identity = Observation<ObjectIdentity>::available({scope, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(static_cast<std::uint64_t>(native.st_dev)),
        NativeIdentifier::from_u64(static_cast<std::uint64_t>(native.st_ino)), generation});
    result.kind = Observation<ObjectKind>::available(kind(native.st_mode));
    result.posix_permissions = Observation<std::uint32_t>::available(static_cast<std::uint32_t>(native.st_mode) & 07777U);
    result.posix_uid = Observation<std::uint64_t>::available(native.st_uid);
    result.posix_gid = Observation<std::uint64_t>::available(native.st_gid);
    result.link_count = Observation<std::uint64_t>::available(native.st_nlink);
    if (S_ISREG(native.st_mode)) {
        if (native.st_size >= 0)
            result.logical_size = Observation<std::uint64_t>::available(static_cast<std::uint64_t>(native.st_size));
        else
            result.logical_size = Observation<std::uint64_t>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, "stat.size"));
        if (native.st_blocks >= 0 && static_cast<std::uint64_t>(native.st_blocks) <= std::numeric_limits<std::uint64_t>::max() / 512U)
            result.allocated_size = Observation<std::uint64_t>::available(static_cast<std::uint64_t>(native.st_blocks) * 512U);
        else
            result.allocated_size = Observation<std::uint64_t>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, "stat.blocks"));
    } else {
        result.logical_size = not_applicable<std::uint64_t>("regular_file.size");
        result.allocated_size = not_applicable<std::uint64_t>("regular_file.allocation");
    }
    return Observation<BasicMetadata>::available(std::move(result));
}

#if defined(__linux__)
Observation<BasicMetadata> statx_metadata(const std::filesystem::path& path, const AdapterScope& scope) {
    struct statx native{};
    unsigned int requested = STATX_BASIC_STATS | STATX_BTIME;
#ifdef STATX_MNT_ID
    requested |= STATX_MNT_ID;
#endif
    if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, requested, &native) != 0) {
        const int code = errno;
        // The older stat interface is a separate documented query, not a claim
        // that omitted statx-only fields were observed. Do not retry denial or
        // I/O failures through a different API and conceal the original error.
        if (code == ENOSYS) return stat_metadata(path, scope);
        return Observation<BasicMetadata>::unavailable(errno_state(code), {ErrorDomain::Posix, code, "statx"});
    }
    BasicMetadata result;
    platform_fields(result);
    result.bsd_flags = not_applicable<std::uint32_t>("bsd_flags");
    result.linux_attributes = Observation<LinuxFileAttributes>::available({native.stx_attributes, native.stx_attributes_mask});
    if ((native.stx_mask & STATX_INO) != 0) {
        result.identity = Observation<ObjectIdentity>::available({scope, IdentityScheme::PosixDeviceInode,
            NativeIdentifier::from_u64(static_cast<std::uint64_t>(makedev(native.stx_dev_major, native.stx_dev_minor))),
            NativeIdentifier::from_u64(native.stx_ino), std::nullopt});
    }
    if ((native.stx_mask & STATX_TYPE) != 0)
        result.kind = Observation<ObjectKind>::available(kind(native.stx_mode));
    if ((native.stx_mask & STATX_MODE) != 0)
        result.posix_permissions = Observation<std::uint32_t>::available(native.stx_mode & 07777U);
    if ((native.stx_mask & STATX_UID) != 0) result.posix_uid = Observation<std::uint64_t>::available(native.stx_uid);
    if ((native.stx_mask & STATX_GID) != 0) result.posix_gid = Observation<std::uint64_t>::available(native.stx_gid);
    if ((native.stx_mask & STATX_NLINK) != 0) result.link_count = Observation<std::uint64_t>::available(native.stx_nlink);
#ifdef STATX_MNT_ID
    if ((native.stx_mask & STATX_MNT_ID) != 0) result.mount_id = Observation<std::uint64_t>::available(native.stx_mnt_id);
#endif
    if ((native.stx_mask & STATX_ATIME) != 0) result.accessed = timestamp(native.stx_atime.tv_sec, native.stx_atime.tv_nsec, "statx.access_time");
    if ((native.stx_mask & STATX_MTIME) != 0) result.modified = timestamp(native.stx_mtime.tv_sec, native.stx_mtime.tv_nsec, "statx.modified_time");
    if ((native.stx_mask & STATX_CTIME) != 0) result.changed = timestamp(native.stx_ctime.tv_sec, native.stx_ctime.tv_nsec, "statx.changed_time");
    if ((native.stx_mask & STATX_BTIME) != 0) result.created = timestamp(native.stx_btime.tv_sec, native.stx_btime.tv_nsec, "statx.birth_time");
    if (result.kind.value() && *result.kind.value() == ObjectKind::RegularFile) {
        if ((native.stx_mask & STATX_SIZE) != 0) result.logical_size = Observation<std::uint64_t>::available(native.stx_size);
        if ((native.stx_mask & STATX_BLOCKS) != 0) {
            if (native.stx_blocks <= std::numeric_limits<std::uint64_t>::max() / 512U)
                result.allocated_size = Observation<std::uint64_t>::available(native.stx_blocks * 512U);
            else
                result.allocated_size = Observation<std::uint64_t>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, "statx.blocks"));
        }
    } else if (result.kind.value()) {
        result.logical_size = not_applicable<std::uint64_t>("regular_file.size");
        result.allocated_size = not_applicable<std::uint64_t>("regular_file.allocation");
    }
    return Observation<BasicMetadata>::available(std::move(result));
}
#endif
} // namespace

Observation<BasicMetadata> NativeFileSystem::inspect_basic(const std::filesystem::path& path) const {
    if (!path.is_absolute() || path.native().find('\0') != std::string::npos)
        return Observation<BasicMetadata>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidPath, "inspect_basic"));
#if defined(__linux__)
    return statx_metadata(path, scope_);
#else
    return stat_metadata(path, scope_);
#endif
}

} // namespace ck::files
