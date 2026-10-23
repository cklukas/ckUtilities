// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "native_filesystem.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#include <sys/attr.h>
#include <sys/mount.h>
#endif
#if defined(__linux__)
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <sys/statvfs.h>
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

Observation<BasicMetadata> stat_metadata(int descriptor, const char* path, bool from_handle, const AdapterScope& scope, int path_flags = AT_SYMLINK_NOFOLLOW) {
    struct stat native{};
    if ((from_handle ? fstat(descriptor, &native) : fstatat(descriptor, path, &native, path_flags)) != 0) {
        const int code = errno;
        return Observation<BasicMetadata>::unavailable(errno_state(code), {ErrorDomain::Posix, code, from_handle ? "fstat" : "fstatat"});
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
        adapter_error(AdapterError::UnsupportedQuery, "stat.birth_time"));
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
Observation<BasicMetadata> statx_metadata(int descriptor, const char* path, int flags, const AdapterScope& scope) {
    struct statx native{};
    unsigned int requested = STATX_BASIC_STATS | STATX_BTIME;
#ifdef STATX_MNT_ID
    requested |= STATX_MNT_ID;
#endif
    if (statx(descriptor, path, flags | AT_NO_AUTOMOUNT, requested, &native) != 0) {
        const int code = errno;
        // The older stat interface is a separate documented query, not a claim
        // that omitted statx-only fields were observed. Do not retry denial or
        // I/O failures through a different API and conceal the original error.
        if (code == ENOSYS) return stat_metadata(descriptor, path, (flags & AT_EMPTY_PATH) != 0, scope, flags & AT_SYMLINK_NOFOLLOW);
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
class OwnedDescriptor final {
public:
    explicit OwnedDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~OwnedDescriptor() { if (descriptor_ >= 0) close(descriptor_); }
    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;
    OwnedDescriptor(OwnedDescriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    int get() const noexcept { return descriptor_; }
    void release() noexcept { descriptor_ = -1; }
private:
    int descriptor_;
};

class PosixOpenedEntry final : public OpenedEntry {
public:
    PosixOpenedEntry(OwnedDescriptor descriptor, AdapterScope scope)
        : descriptor_(std::move(descriptor)), scope_(std::move(scope)) {}
    Observation<LinkMetadata> read_link_metadata(InventoryLimits limits = {}) const override {
        using Result = Observation<LinkMetadata>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidLimit, "link.limits"));
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!metadata.value()->kind.value()) { const auto& e = *metadata.value()->kind.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (*metadata.value()->kind.value() != ObjectKind::SymbolicLink)
            return Result::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::NotApplicable, "link.object_kind"));
        const auto query = [&](std::span<std::byte> buffer) -> detail::LinkReadReply {
#if defined(__linux__)
            const auto count = readlinkat(descriptor_.get(), "", reinterpret_cast<char*>(buffer.data()), buffer.size());
            constexpr const char* operation = "readlinkat.descriptor";
#else
            ssize_t count = -1;
            if (__builtin_available(macos 13.0, *))
                count = freadlink(descriptor_.get(), reinterpret_cast<char*>(buffer.data()), buffer.size());
            else return UnavailableObservation{ObservationState::Unsupported, adapter_error(AdapterError::UnsupportedQuery, "freadlink.availability")};
            constexpr const char* operation = "freadlink";
#endif
            if (count >= 0) return static_cast<std::size_t>(count);
            const int code = errno;
            return UnavailableObservation{errno_state(code), {ErrorDomain::Posix, code, operation}};
        };
        auto target = detail::read_posix_link(query, limits);
        if (!target.value()) { const auto& e = *target.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available({metadata.value()->identity, std::move(target).take_value()});
    }
    Observation<StorageInfo> inspect_storage() const override {
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Observation<StorageInfo>::unavailable(e.state, e.error); }
#if defined(__linux__)
        return storage_from_basic(*metadata.value(), StorageModel::LinuxStatx);
#else
        return storage_from_basic(*metadata.value(), StorageModel::DarwinStatFlags);
#endif
    }
    Observation<BasicMetadata> inspect_basic() const override {
#if defined(__linux__)
        return statx_metadata(descriptor_.get(), "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, scope_);
#else
        return stat_metadata(descriptor_.get(), nullptr, true, scope_);
#endif
    }
#if defined(__APPLE__)
    Observation<DataChannelCatalog> inspect_data_channels(InventoryLimits limits = {}) const override {
        using Result = Observation<DataChannelCatalog>;
        if (!limits.valid())
            return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidLimit, "channels.limits"));
        const auto metadata = inspect_basic();
        if (!metadata.value()) {
            const auto& detail = *metadata.unavailable_detail();
            return Result::unavailable(detail.state, detail.error);
        }
        DataChannelCatalog result;
        if (const auto guarded = provider_guard(*metadata.value())) return Result::unavailable(guarded->state, guarded->error);
        result.coverage = DataChannelCoverage::MacResourceFork;
        result.owner = metadata.value()->identity;
        constexpr const char* resource_fork = "com.apple.ResourceFork";
        const auto size = fgetxattr(descriptor_.get(), resource_fork, nullptr, 0, 0, 0);
        if (size < 0) {
            const int code = errno;
            if (code != ENOATTR)
                return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "fgetxattr.resource_fork_size"});
        } else {
            result.channels.push_back({*NativeFeatureIdentifier::from_posix(resource_fork), DataChannelKind::ResourceFork,
                Observation<std::uint64_t>::available(static_cast<std::uint64_t>(size)), {}});
        }
        return Result::available(std::move(result));
    }
#endif
    Observation<AttributeCatalog> inspect_attributes(InventoryLimits limits = {}) const override {
        using Result = Observation<AttributeCatalog>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidLimit, "attributes.limits"));
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
#if defined(__APPLE__)
        if (const auto guarded = provider_guard(*metadata.value())) return Result::unavailable(guarded->state, guarded->error);
#endif
        const auto proxy = attribute_proxy();
        if (!proxy.value()) { const auto& e = *proxy.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto bytes = detail::read_attribute_bytes([&](std::span<std::byte> buffer) {
#if defined(__linux__)
            const auto count = listxattr(proxy.value()->c_str(), reinterpret_cast<char*>(buffer.data()), buffer.size());
#else
            const auto count = flistxattr(descriptor_.get(), reinterpret_cast<char*>(buffer.data()), buffer.size(), XATTR_SHOWCOMPRESSION);
#endif
            return attribute_reply(count, "listxattr.names");
        }, limits);
        if (!bytes.value()) { const auto& e = *bytes.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        auto decoded = detail::decode_posix_attribute_names(*bytes.value(), limits);
        if (!decoded.value()) { const auto& e = *decoded.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        AttributeCatalog result;
        result.owner = metadata.value()->identity;
        result.coverage = AttributeCoverage::CredentialsVisiblePosix;
        auto names = std::move(decoded).take_value();
        for (auto& id : names) {
            const auto size = attribute_size(*id.posix_bytes(), *proxy.value());
            result.attributes.push_back({std::move(id), AttributeRole::Opaque, size,
                Observation<std::uint32_t>::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::NotApplicable, "attributes.flags"))});
            result.attributes.back().role = attribute_role(result.attributes.back().identifier);
        }
        const auto checked = attribute_proxy();
        if (!checked.value()) { const auto& e = *checked.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available(std::move(result));
    }
    Observation<AttributeValue> read_attribute(const NativeFeatureIdentifier& id, InventoryLimits limits = {}) const override {
        using Result = Observation<AttributeValue>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidLimit, "attributes.limits"));
        const auto* name = id.posix_bytes();
        if (!name || !NativeFeatureIdentifier::from_posix(*name))
            return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidIdentifier, "attributes.identifier"));
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
#if defined(__APPLE__)
        if (const auto guarded = provider_guard(*metadata.value())) return Result::unavailable(guarded->state, guarded->error);
#endif
        const auto proxy = attribute_proxy();
        if (!proxy.value()) { const auto& e = *proxy.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        auto bytes = detail::read_attribute_bytes([&](std::span<std::byte> buffer) {
#if defined(__linux__)
            const auto count = getxattr(proxy.value()->c_str(), name->c_str(), buffer.data(), buffer.size());
#else
            const auto count = fgetxattr(descriptor_.get(), name->c_str(), buffer.data(), buffer.size(), 0, XATTR_SHOWCOMPRESSION);
#endif
            return attribute_reply(count, "getxattr.value");
        }, limits);
        if (!bytes.value()) { const auto& e = *bytes.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto checked = attribute_proxy();
        if (!checked.value()) { const auto& e = *checked.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available({id, std::move(bytes).take_value(), metadata.value()->identity,
            Observation<std::uint32_t>::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::NotApplicable, "attributes.flags"))});
    }
    Observation<NativeSecurityValue> read_security(SecurityPart part, InventoryLimits limits = {}) const override {
        using Result = Observation<NativeSecurityValue>;
        if (!valid_security_part(part) || !limits.valid())
            return Result::unavailable(ObservationState::Failed, adapter_error(!valid_security_part(part) ? AdapterError::InvalidIdentifier : AdapterError::InvalidLimit, "security.query"));
#if defined(__linux__)
        if (part != SecurityPart::LinuxAccessAcl && part != SecurityPart::LinuxDefaultAcl)
            return Result::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::NotApplicable, "security.model"));
        const auto id = *NativeFeatureIdentifier::from_posix(part == SecurityPart::LinuxAccessAcl ? "system.posix_acl_access" : "system.posix_acl_default");
        auto queried = read_attribute(id, limits);
        if (!queried.value()) {
            const auto& e = *queried.unavailable_detail();
            if (e.error.domain == ErrorDomain::Posix && e.error.code == ENODATA) {
                const auto metadata = inspect_basic();
                if (!metadata.value()) { const auto& error = *metadata.unavailable_detail(); return Result::unavailable(error.state, error.error); }
                return Result::available({part, SecurityEncoding::LinuxPosixAclXattrV2, SecurityPresence::Absent, {}, metadata.value()->identity, 0});
            }
            return Result::unavailable(e.state, e.error);
        }
        auto value = std::move(queried).take_value();
        const auto checked = detail::validate_linux_acl(value.bytes, limits);
        if (!checked.value()) { const auto& e = *checked.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available({part, SecurityEncoding::LinuxPosixAclXattrV2, SecurityPresence::Present, std::move(value.bytes), std::move(value.owner), 0});
#else
        if (part != SecurityPart::DarwinExtendedAcl)
            return Result::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::NotApplicable, "security.model"));
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto native_error = [](int code, const char* operation) {
            return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, operation});
        };
        struct FileSecurityOwner {
            filesec_t value;
            ~FileSecurityOwner() { if (value) filesec_free(value); }
        } security{filesec_init()};
        if (!security.value) return native_error(errno, "filesec_init");
        struct stat native{};
        if (fstatx_np(descriptor_.get(), &native, security.value) != 0) return native_error(errno, "fstatx_np.security");
        int present = 0;
        if (filesec_query_property(security.value, FILESEC_ACL, &present) != 0) return native_error(errno, "filesec_query_property.acl");
        if (!present) {
            // Unlike acl_get_fd_np's overloaded ENOENT, this is a successful
            // object query with an explicitly missing ACL property. Qualify
            // native extended-security support before calling it absent.
            errno = 0;
            const auto supported = fpathconf(descriptor_.get(), _PC_EXTENDED_SECURITY_NP);
            if (supported < 0) {
                const int code = errno;
                if (code != 0) return native_error(code, "fpathconf.extended_security");
                return Result::unavailable(ObservationState::Unknown, adapter_error(AdapterError::UnsupportedQuery, "fpathconf.extended_security"));
            }
            if (supported == 0) return Result::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::UnsupportedQuery, "security.extended_acl"));
            return Result::available({part, SecurityEncoding::DarwinExternalAcl, SecurityPresence::Absent, {}, metadata.value()->identity, 0});
        }
        struct AclOwner {
            acl_t value = nullptr;
            ~AclOwner() { if (value) acl_free(value); }
        } acl;
        if (filesec_get_property(security.value, FILESEC_ACL, &acl.value) != 0) return native_error(errno, "filesec_get_property.acl");
        if (!acl.value) return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, "filesec_get_property.null_acl"));
        if (acl_valid(acl.value) != 0) return native_error(errno, "acl_valid");
        std::size_t entries = 0;
        acl_entry_t entry{};
        int selector = ACL_FIRST_ENTRY;
        for (;;) {
            if (acl_get_entry(acl.value, selector, &entry) != 0) {
                const int code = errno;
                if (code != EINVAL) return native_error(code, "acl_get_entry");
                break; // Darwin's documented end-of-working-ACL result.
            }
            if (entries == limits.max_records)
                return Result::unavailable(ObservationState::Unknown, adapter_error(AdapterError::InventoryLimit, "security.records"));
            ++entries;
            selector = ACL_NEXT_ENTRY;
        }
        const auto size = acl_size(acl.value);
        if (size < 0) return native_error(errno, "acl_size");
        if (static_cast<std::uint64_t>(size) > limits.max_native_bytes)
            return Result::unavailable(ObservationState::Unknown, adapter_error(AdapterError::InventoryLimit, "security.bytes"));
        NativeBytes bytes(static_cast<std::size_t>(size));
        const auto copied = acl_copy_ext(bytes.data(), acl.value, size);
        if (copied < 0) return native_error(errno, "acl_copy_ext");
        if (copied != size) return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, "acl_copy_ext.size"));
        return Result::available({part, SecurityEncoding::DarwinExternalAcl, SecurityPresence::Present, std::move(bytes), metadata.value()->identity, 0});
#endif
    }
private:
#if defined(__APPLE__)
    static std::optional<UnavailableObservation> provider_guard(const BasicMetadata& metadata) {
        const auto review = provider_review_required(metadata, StorageModel::DarwinStatFlags);
        if (!review.value()) return *review.unavailable_detail();
        if (*review.value()) return UnavailableObservation{ObservationState::Unknown,
            adapter_error(AdapterError::ProviderReviewRequired, "attributes.provider_review")};
        return std::nullopt;
    }
#endif
    static detail::BufferReply attribute_reply(ssize_t count, const char* operation) {
        if (count >= 0) return static_cast<std::size_t>(count);
        const int code = errno;
        const NativeError error{ErrorDomain::Posix, code, operation};
        if (code == ERANGE) return detail::BufferTooSmall{error};
#if defined(__linux__)
        const auto state = code == ENODATA ? ObservationState::Gone : errno_state(code);
#else
        const auto state = code == ENOATTR ? ObservationState::Gone : errno_state(code);
#endif
        return UnavailableObservation{state, error};
    }
    Observation<std::uint64_t> attribute_size(const std::string& name, const std::string& proxy) const {
#if defined(__linux__)
        const auto count = getxattr(proxy.c_str(), name.c_str(), nullptr, 0);
#else
        (void)proxy;
        const auto count = fgetxattr(descriptor_.get(), name.c_str(), nullptr, 0, 0, XATTR_SHOWCOMPRESSION);
#endif
        auto reply = attribute_reply(count, "getxattr.size");
        if (const auto* size = std::get_if<std::size_t>(&reply)) return Observation<std::uint64_t>::available(*size);
        if (const auto* error = std::get_if<UnavailableObservation>(&reply))
            return Observation<std::uint64_t>::unavailable(error->state, error->error);
        return Observation<std::uint64_t>::unavailable(ObservationState::Changed, std::get<detail::BufferTooSmall>(reply).error);
    }
    Observation<std::string> attribute_proxy() const {
        using Result = Observation<std::string>;
#if defined(__linux__)
        // The kernel's descriptor namespace refers to the held object, including
        // after rename/unlink. No source pathname or data-read fd is opened.
        struct statfs native{};
        if (statfs("/proc/self/fd", &native) != 0) {
            const int code = errno;
            return Result::unavailable(code == ENOENT || code == ENOTDIR ? ObservationState::Unknown : errno_state(code),
                {ErrorDomain::Posix, code, "attributes.proc_namespace"});
        }
        if (static_cast<unsigned long>(native.f_type) != static_cast<unsigned long>(PROC_SUPER_MAGIC))
            return Result::unavailable(ObservationState::Unsupported, adapter_error(AdapterError::UnsupportedQuery, "attributes.proc_namespace"));
        const auto path = std::string("/proc/self/fd/") + std::to_string(descriptor_.get());
        const auto original = inspect_basic();
        const auto via_proxy = statx_metadata(AT_FDCWD, path.c_str(), 0, scope_);
        if (!original.value()) { const auto& e = *original.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!via_proxy.value()) {
            const auto& e = *via_proxy.unavailable_detail();
            return Result::unavailable(e.state == ObservationState::Gone ? ObservationState::Unknown : e.state, e.error);
        }
        const auto* expected = original.value()->identity.value();
        const auto* actual = via_proxy.value()->identity.value();
        const auto comparison = expected && actual ? compare_identity(*expected, *actual) : IdentityComparison::Unknown;
        if (comparison != IdentityComparison::Same)
            return Result::unavailable(comparison == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
                adapter_error(AdapterError::UnqualifiedIdentity, "attributes.descriptor_identity"));
        return Result::available(path);
#else
        return Result::available({}); // Darwin uses only the owned fd.
#endif
    }
    OwnedDescriptor descriptor_;
    AdapterScope scope_;
};

struct DirectoryCloser {
    void operator()(DIR* directory) const noexcept { closedir(directory); }
};
using OwnedDirectory = std::unique_ptr<DIR, DirectoryCloser>;

class PosixDirectoryCursor final : public DirectoryCursor {
public:
    PosixDirectoryCursor(OwnedDirectory directory, Observation<ObjectIdentity> identity, AdapterScope scope)
        : DirectoryCursor(std::move(identity)), directory_(std::move(directory)), scope_(std::move(scope)) {}
    Observation<LocationInfo> inspect_location() const override {
        LocationInfo result;
        const auto fd = dirfd(directory_.get());
        const auto metadata = stat_metadata(fd, nullptr, true, scope_);
        if (!metadata.value()) {
            const auto& e = *metadata.unavailable_detail();
            return Observation<LocationInfo>::unavailable(e.state, e.error);
        }
        result.directory = metadata.value()->identity;
        result.volume_query_serial = not_applicable<std::uint64_t>("volume.query_serial");
        result.windows_component_utf16_max = not_applicable<std::uint64_t>("volume.windows_component_limit");
        result.windows_directory_case_flags = not_applicable<std::uint32_t>("volume.windows_directory_case_flags");
        const auto limit = [&](int name, const char* operation) {
            errno = 0;
            const auto value = fpathconf(fd, name);
            const int code = errno;
            if (value < 0) return Observation<std::uint64_t>::unavailable(code == 0 ? ObservationState::Unknown : errno_state(code),
                {ErrorDomain::Posix, code, operation});
            if (value == 0) return Observation<std::uint64_t>::unavailable(ObservationState::Unknown,
                adapter_error(AdapterError::InvalidNativeResult, operation));
            return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(value));
        };
        result.posix_name_max = limit(_PC_NAME_MAX, "fpathconf.name_max");
        result.posix_path_max = limit(_PC_PATH_MAX, "fpathconf.path_max");
#if defined(__linux__)
        result.family = LocationQueryFamily::LinuxDescriptor;
        result.posix_name_chars_max = not_applicable<std::uint64_t>("volume.name_chars_max");
        result.features = Observation<NativeVolumeFeatures>::unavailable(ObservationState::Unsupported,
            adapter_error(AdapterError::UnsupportedQuery, "volume.feature_advertisements"));
        result.case_sensitive_directory = Observation<bool>::unavailable(ObservationState::Unsupported,
            adapter_error(AdapterError::UnsupportedQuery, "volume.directory_case"));
        result.case_preserving_names = Observation<bool>::unavailable(ObservationState::Unsupported,
            adapter_error(AdapterError::UnsupportedQuery, "volume.case_preservation"));
        result.filesystem_name = Observation<NativeFeatureIdentifier>::unavailable(ObservationState::Unsupported,
            adapter_error(AdapterError::UnsupportedQuery, "volume.filesystem_name"));
        struct statvfs space{};
        if (fstatvfs(fd, &space) == 0) {
            result.native_mount_flags = Observation<std::uint64_t>::available(space.f_flag);
            result.read_only_mount = Observation<bool>::available((space.f_flag & ST_RDONLY) != 0);
            result.space_accounting_unit = detail::space_bytes(1, space.f_frsize);
            result.total_bytes = detail::space_bytes(space.f_blocks, space.f_frsize);
            result.free_bytes = detail::space_bytes(space.f_bfree, space.f_frsize);
            result.available_bytes = detail::space_bytes(space.f_bavail, space.f_frsize);
        } else {
            const int code = errno;
            const NativeError error{ErrorDomain::Posix, code, "fstatvfs"};
            result.native_mount_flags = Observation<std::uint64_t>::unavailable(errno_state(code), error);
            result.read_only_mount = Observation<bool>::unavailable(errno_state(code), error);
            result.space_accounting_unit = result.total_bytes = result.free_bytes = result.available_bytes =
                Observation<std::uint64_t>::unavailable(errno_state(code), error);
        }
        struct statfs native{};
#else
        result.family = LocationQueryFamily::DarwinDescriptor;
        result.posix_name_chars_max = limit(_PC_NAME_CHARS_MAX, "fpathconf.name_chars_max");
        const auto boolean = [&](int name, const char* operation) {
            errno = 0;
            const auto value = fpathconf(fd, name);
            const int code = errno;
            if (value < 0) return Observation<bool>::unavailable(code == 0 ? ObservationState::Unknown : errno_state(code),
                {ErrorDomain::Posix, code, operation});
            if (value > 1) return Observation<bool>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, operation));
            return Observation<bool>::available(value != 0);
        };
        result.case_sensitive_directory = boolean(_PC_CASE_SENSITIVE, "fpathconf.case_sensitive");
        result.case_preserving_names = boolean(_PC_CASE_PRESERVING, "fpathconf.case_preserving");
        struct CapabilityRecord { std::uint32_t length; vol_capabilities_attr_t caps; } capabilities{};
        static_assert(offsetof(CapabilityRecord, caps) == 4 && sizeof(CapabilityRecord) == 36);
        struct attrlist attributes{};
        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
        if (fgetattrlist(fd, &attributes, &capabilities, sizeof(capabilities), 0) == 0) {
            if (capabilities.length == sizeof(capabilities)) {
                NativeVolumeFeatures features;
                features.domain = VolumeFeatureDomain::DarwinVolumeCapabilities;
                for (std::size_t group = 0; group < 4; ++group) {
                    features.value[group] = capabilities.caps.capabilities[group];
                    features.valid[group] = capabilities.caps.valid[group];
                }
                result.features = Observation<NativeVolumeFeatures>::available(features);
            } else result.features = Observation<NativeVolumeFeatures>::unavailable(ObservationState::Failed,
                adapter_error(AdapterError::InvalidNativeResult, "fgetattrlist.volume_capabilities_size"));
        } else {
            const int code = errno;
            result.features = Observation<NativeVolumeFeatures>::unavailable(code == EINVAL ? ObservationState::Unsupported : errno_state(code),
                {ErrorDomain::Posix, code, "fgetattrlist.volume_capabilities"});
        }
        struct statfs native{};
#endif
        if (fstatfs(fd, &native) == 0) {
            result.filesystem_type_code = Observation<std::uint64_t>::available(static_cast<std::uint64_t>(native.f_type));
            NativeIdentifier id;
            static_assert(sizeof(native.f_fsid) <= sizeof(id.bytes));
            std::memcpy(id.bytes.data(), &native.f_fsid, sizeof(native.f_fsid));
            result.filesystem_query_id = Observation<NativeIdentifier>::available(id);
#if defined(__APPLE__)
            result.native_mount_flags = Observation<std::uint64_t>::available(native.f_flags);
            result.read_only_mount = Observation<bool>::available((native.f_flags & MNT_RDONLY) != 0);
            result.space_accounting_unit = detail::space_bytes(1, native.f_bsize);
            result.total_bytes = detail::space_bytes(native.f_blocks, native.f_bsize);
            result.free_bytes = detail::space_bytes(native.f_bfree, native.f_bsize);
            result.available_bytes = detail::space_bytes(native.f_bavail, native.f_bsize);
            const auto length = strnlen(native.f_fstypename, sizeof(native.f_fstypename));
            auto name = length < sizeof(native.f_fstypename) ? NativeFeatureIdentifier::from_posix(std::string(native.f_fstypename, length)) : std::nullopt;
            result.filesystem_name = name ? Observation<NativeFeatureIdentifier>::available(std::move(*name)) :
                Observation<NativeFeatureIdentifier>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidNativeResult, "fstatfs.filesystem_name"));
#endif
        } else {
            const int code = errno;
            const NativeError error{ErrorDomain::Posix, code, "fstatfs"};
            result.filesystem_type_code = Observation<std::uint64_t>::unavailable(errno_state(code), error);
            result.filesystem_query_id = Observation<NativeIdentifier>::unavailable(errno_state(code), error);
#if defined(__APPLE__)
            result.filesystem_name = Observation<NativeFeatureIdentifier>::unavailable(errno_state(code), error);
            result.native_mount_flags = Observation<std::uint64_t>::unavailable(errno_state(code), error);
            result.read_only_mount = Observation<bool>::unavailable(errno_state(code), error);
            result.space_accounting_unit = result.total_bytes = result.free_bytes = result.available_bytes =
                Observation<std::uint64_t>::unavailable(errno_state(code), error);
#endif
        }
        return Observation<LocationInfo>::available(std::move(result));
    }
    Observation<std::unique_ptr<DirectoryCursor>> open_directory_child(const NativeName& name) const override {
        using Result = Observation<std::unique_ptr<DirectoryCursor>>;
        const auto* spelling = name.posix_bytes();
        if (!name.is_valid() || !spelling) return Result::unavailable(ObservationState::Failed,
            adapter_error(AdapterError::InvalidIdentifier, "open_directory_child.name"));
        OwnedDescriptor fd(openat(dirfd(directory_.get()), spelling->c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
        if (fd.get() < 0) {
            const int code = errno;
            return Result::unavailable(code == ENOTDIR ? ObservationState::Failed : errno_state(code),
                {ErrorDomain::Posix, code, "openat.directory_child"});
        }
        struct stat native{};
        if (fstat(fd.get(), &native) != 0) {
            const int code = errno;
            return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "fstat.directory_child"});
        }
        std::optional<std::uint64_t> generation;
#if defined(__APPLE__)
        generation = native.st_gen;
#endif
        auto observed = Observation<ObjectIdentity>::available({scope_, IdentityScheme::PosixDeviceInode,
            NativeIdentifier::from_u64(static_cast<std::uint64_t>(native.st_dev)),
            NativeIdentifier::from_u64(static_cast<std::uint64_t>(native.st_ino)), generation});
        OwnedDirectory child(fdopendir(fd.get()));
        if (!child) {
            const int code = errno;
            return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "fdopendir.child"});
        }
        fd.release();
        return Result::available(std::make_unique<PosixDirectoryCursor>(std::move(child), std::move(observed), scope_));
    }
    Observation<ChildLookup> lookup_child(const NativeName& name, ChildLookupMode mode) const override {
        using Result=Observation<ChildLookup>;
        if(!name.is_valid()||!name.posix_bytes()||!valid_child_lookup_mode(mode))
            return Result::unavailable(ObservationState::Failed,adapter_error(AdapterError::InvalidIdentifier,"lookup_child.input"));
        if(mode!=ChildLookupMode::NativeDefault)
            return Result::unavailable(ObservationState::Unsupported,adapter_error(AdapterError::UnsupportedQuery,"lookup_child.mode"));
        auto opened=open_child(name,EntryAccess::MetadataOnly);
        if(opened.value()) {
            auto owned=std::move(opened).take_value();
            if(!owned)return Result::unavailable(ObservationState::Failed,adapter_error(AdapterError::InvalidNativeResult,"lookup_child.null_entry"));
            return Result::available(ChildLookup::found({name,mode,identity()},std::move(owned)));
        }
        const auto& e=*opened.unavailable_detail();
        if(detail::definite_child_absence(e,detail::ChildLookupFamily::PosixAt))
            return Result::available(ChildLookup::missing({name,mode,identity()},e.error));
        return Result::unavailable(e.state,e.error);
    }
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name, EntryAccess access = EntryAccess::MetadataOnly) const override {
        if (!valid_entry_access(access)) return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Failed,
            adapter_error(AdapterError::InvalidAccess, "open_child.access"));
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        const auto* spelling = name.posix_bytes();
        if (!spelling || !name.is_valid())
            return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidPath, "open_child.name"));
#if defined(__linux__)
        constexpr int flags = O_PATH | O_NOFOLLOW | O_CLOEXEC;
#else
        // A public event-only descriptor observes identity/metadata; it is not a
        // data-read API and does not prevent volume unmount. The open can still
        // require read permission: preserve Denied rather than bypassing it. O_SYMLINK
        // opens the final symlink itself, including a dangling link.
        constexpr int flags = O_EVTONLY | O_SYMLINK | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
#endif
        OwnedDescriptor descriptor(openat(dirfd(directory_.get()), spelling->c_str(), flags));
        if (descriptor.get() < 0) {
            const int code = errno;
            return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "openat.child"});
        }
        return Result::available(std::make_unique<PosixOpenedEntry>(std::move(descriptor), scope_));
    }
private:
    Observation<DirectoryStep> next_step() override {
        using Result = Observation<DirectoryStep>;
        errno = 0;
        const auto* entry = readdir(directory_.get());
        if (!entry) {
            const int code = errno;
            if (code == 0) return Result::available(DirectoryEnd{});
            return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "readdir"});
        }
        constexpr auto name_offset = offsetof(struct dirent, d_name);
        if (entry->d_reclen <= name_offset)
            return Result::unavailable(ObservationState::Failed,
                adapter_error(AdapterError::InvalidNativeResult, "readdir.record"));
        const auto capacity = static_cast<std::size_t>(entry->d_reclen) - name_offset;
        const auto length = strnlen(entry->d_name, capacity);
        if (length == capacity)
            return Result::unavailable(ObservationState::Failed,
                adapter_error(AdapterError::InvalidNativeResult, "readdir.name_length"));
        const std::string_view spelling(entry->d_name, length);
        if (spelling == "." || spelling == "..") return Result::available(DirectorySkip{});
        auto name = NativeName::from_posix(std::string(spelling));
        if (!name)
            return Result::unavailable(ObservationState::Failed,
                adapter_error(AdapterError::InvalidNativeResult, "readdir.name"));
        return Result::available(std::move(*name));
    }
    OwnedDirectory directory_;
    AdapterScope scope_;
};

} // namespace

Observation<BasicMetadata> NativeFileSystem::inspect_basic(const std::filesystem::path& path) const {
    if (!path.is_absolute() || path.native().find('\0') != std::string::npos)
        return Observation<BasicMetadata>::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidPath, "inspect_basic"));
#if defined(__linux__)
    return statx_metadata(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, scope_);
#else
    return stat_metadata(AT_FDCWD, path.c_str(), false, scope_);
#endif
}

Observation<std::unique_ptr<DirectoryCursor>> NativeFileSystem::open_directory(
    const std::filesystem::path& path, DirectoryLinkPolicy policy) const {
    using Result = Observation<std::unique_ptr<DirectoryCursor>>;
    if (!path.is_absolute() || path.native().find('\0') != std::string::npos ||
        (policy != DirectoryLinkPolicy::RejectFinalLink && policy != DirectoryLinkPolicy::FollowFinalLink))
        return Result::unavailable(ObservationState::Failed, adapter_error(AdapterError::InvalidPath, "open_directory"));
    auto spelling = path.native();
    // A trailing slash must not turn an explicitly rejected final symlink into
    // a followed parent component. Preserve all-slash root spellings verbatim.
    if (spelling.find_first_not_of('/') != std::string::npos)
        while (spelling.size() > 1 && spelling.back() == '/') spelling.pop_back();
    OwnedDescriptor fd(open(spelling.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
        (policy == DirectoryLinkPolicy::RejectFinalLink ? O_NOFOLLOW : 0)));
    if (fd.get() < 0) {
        const int code = errno;
        // The requested directory can instead be an existing non-directory
        // (or a refused final link). ENOTDIR here does not prove it vanished.
        const auto state = code == ENOTDIR ? ObservationState::Failed : errno_state(code);
        return Result::unavailable(state, {ErrorDomain::Posix, code, "open.directory"});
    }
    struct stat native{};
    if (fstat(fd.get(), &native) != 0) {
        const int code = errno;
        return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "fstat.directory"});
    }
    std::optional<std::uint64_t> generation;
#if defined(__APPLE__)
    generation = native.st_gen;
#endif
    auto identity = Observation<ObjectIdentity>::available({scope_, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(static_cast<std::uint64_t>(native.st_dev)),
        NativeIdentifier::from_u64(static_cast<std::uint64_t>(native.st_ino)), generation});
    OwnedDirectory directory(fdopendir(fd.get()));
    if (!directory) {
        const int code = errno;
        return Result::unavailable(errno_state(code), {ErrorDomain::Posix, code, "fdopendir"});
    }
    fd.release(); // fdopendir now owns the descriptor, including CLOEXEC.
    return Result::available(std::make_unique<PosixDirectoryCursor>(std::move(directory), std::move(identity), scope_));
}
} // namespace ck::files
