// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "observation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ck::files {

// Unique within the owning adapter lifetime, without global counters or time.
// A copied token names the same adapter; a new token does not establish that
// two native observations came from the same host/identity namespace.
class AdapterScope final {
public:
    AdapterScope() : token_(std::make_shared<const Token>()) {}
    bool is_valid() const noexcept { return token_ != nullptr; }
    friend bool operator==(const AdapterScope&, const AdapterScope&) = default;
private:
    struct Token {};
    std::shared_ptr<const Token> token_;
};

struct NativeIdentifier {
    std::array<std::byte, 16> bytes{};
    static NativeIdentifier from_u64(std::uint64_t value) noexcept;
    friend bool operator==(const NativeIdentifier&, const NativeIdentifier&) = default;
};

enum class IdentityScheme { Unknown, PosixDeviceInode, WindowsFileId128, WindowsFileIndex64 };
struct ObjectIdentity {
    AdapterScope scope;
    IdentityScheme scheme = IdentityScheme::Unknown;
    NativeIdentifier volume;
    NativeIdentifier object;
    std::optional<std::uint64_t> generation;
};

enum class IdentityComparison { Same, Different, Unknown };
// Native-ID comparison only. Same is not content equality, a locked snapshot,
// a durable identity across remount/restart, or authority to unlink a name.
IdentityComparison compare_identity(const ObjectIdentity& left, const ObjectIdentity& right) noexcept;

class FileTimestamp final {
public:
    static std::optional<FileTimestamp> from_unix(std::int64_t seconds, std::uint32_t nanos) noexcept;
    static FileTimestamp from_windows_ticks(std::uint64_t ticks) noexcept;
    std::int64_t seconds() const noexcept { return seconds_; }
    std::uint32_t nanoseconds() const noexcept { return nanos_; }
    friend bool operator==(const FileTimestamp&, const FileTimestamp&) = default;
private:
    FileTimestamp(std::int64_t seconds, std::uint32_t nanos) : seconds_(seconds), nanos_(nanos) {}
    std::int64_t seconds_;
    std::uint32_t nanos_;
};

enum class ObjectKind { RegularFile, Directory, SymbolicLink, Junction, ReparsePoint, Fifo, Socket, CharacterDevice, BlockDevice, Other };

struct LinuxFileAttributes {
    std::uint64_t value = 0;
    // Only bits in known_mask have an observed supported value.
    std::uint64_t known_mask = 0;
};

// Basic native observations, not a complete preservation inventory. Unknown
// defaults do not assert absence of ACLs, streams, xattrs or provider content.
// No timestamp or stat/ID observation proves content stability.
struct BasicMetadata {
    Observation<ObjectIdentity> identity;
    Observation<ObjectKind> kind;
    Observation<std::uint64_t> logical_size;   // regular-file data only
    Observation<std::uint64_t> allocated_size; // regular-file allocation only
    Observation<std::uint64_t> link_count;
    Observation<std::uint64_t> posix_uid;
    Observation<std::uint64_t> posix_gid;
    Observation<std::uint32_t> posix_permissions; // permission/special bits, not type
    Observation<std::uint32_t> bsd_flags;
    Observation<std::uint32_t> windows_attributes;
    Observation<std::uint32_t> reparse_tag;
    Observation<LinuxFileAttributes> linux_attributes;
    Observation<std::uint64_t> mount_id;
    Observation<FileTimestamp> accessed;
    Observation<FileTimestamp> modified;
    Observation<FileTimestamp> changed;
    Observation<FileTimestamp> created;
};

} // namespace ck::files
