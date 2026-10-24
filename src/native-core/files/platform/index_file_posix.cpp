// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_index_file.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#include <sys/mount.h>
#include <sys/random.h>
#else
#include <sys/vfs.h>
#include <linux/magic.h>
#endif
namespace ck::files {
namespace {
class Fd final {
  public:
    explicit Fd(int fd) : value(fd) {
    }
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int release() noexcept {
        const int fd = value;
        value = -1;
        return fd;
    }
    int value;
};
ObservationState state(int code) {
    if (code == ENOENT || code == ENOTDIR)
        return ObservationState::Gone;
    if (code == EACCES || code == EPERM)
        return ObservationState::Denied;
    if (code == ENOSYS || code == ENOTSUP)
        return ObservationState::Unsupported;
    return ObservationState::Failed;
}
template <class T> Observation<T> failure(int code, const char *op) {
    return Observation<T>::unavailable(state(code), {ErrorDomain::Posix, code, op});
}
template <class T>
Observation<T> rejected(AdapterError code, const char *op, ObservationState s = ObservationState::Failed) {
    return Observation<T>::unavailable(s, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op});
}
Observation<bool> private_acl(int fd) {
#ifdef __APPLE__
    struct SecurityOwner {
        filesec_t value = filesec_init();
        ~SecurityOwner() {
            if (value)
                filesec_free(value);
        }
    } security;
    if (!security.value)
        return failure<bool>(errno, "index_file.filesec_init");
    struct stat info{};
    if (fstatx_np(fd, &info, security.value) != 0)
        return failure<bool>(errno, "index_file.fstatx");
    int present = 0;
    if (filesec_query_property(security.value, FILESEC_ACL, &present) != 0)
        return failure<bool>(errno, "index_file.acl_property");
    if (!present) {
        errno = 0;
        const auto supported = fpathconf(fd, _PC_EXTENDED_SECURITY_NP);
        if (supported < 0 && errno)
            return failure<bool>(errno, "index_file.extended_security");
        if (supported <= 0)
            return rejected<bool>(AdapterError::UnsupportedQuery, "index_file.extended_security",
                                  ObservationState::Unsupported);
        return Observation<bool>::available(true);
    }
    acl_t acl{};
    if (filesec_get_property(security.value, FILESEC_ACL, &acl) != 0)
        return failure<bool>(errno, "index_file.acl_get_property");
    if (!acl)
        return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.null_acl");
    if (acl_valid(acl) != 0) {
        const int code = errno;
        acl_free(acl);
        return failure<bool>(code, "index_file.acl_valid");
    }
    acl_entry_t entry{};
    errno = 0;
    const int found = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    const int code = errno;
    acl_free(acl);
    if (found < 0 && code != EINVAL)
        return failure<bool>(code, "index_file.acl_get_entry");
    // Darwin returns -1/EINVAL for an empty ACL, 0 for a present entry.
    if (found == 0)
        return rejected<bool>(AdapterError::InvalidAccess, "index_file.extended_acl", ObservationState::Denied);
    if (found != -1 || code != EINVAL)
        return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.acl_result");
#else
    (void)fd; // Qualified Linux filesystems enforce the POSIX ACL mask via mode.
#endif
    return Observation<bool>::available(true);
}
Observation<bool> qualified_volume(int fd) {
    struct statfs volume{};
    if (fstatfs(fd, &volume) != 0)
        return failure<bool>(errno, "index_file.fstatfs");
#ifdef __APPLE__
    if (!(volume.f_flags & MNT_LOCAL) || (volume.f_flags & MNT_IGNORE_OWNERSHIP) ||
        (std::string_view(volume.f_fstypename) != "apfs" && std::string_view(volume.f_fstypename) != "hfs"))
#else
    if (volume.f_type != EXT4_SUPER_MAGIC && volume.f_type != BTRFS_SUPER_MAGIC && volume.f_type != XFS_SUPER_MAGIC &&
        volume.f_type != TMPFS_MAGIC)
#endif
        return rejected<bool>(AdapterError::UnsupportedQuery, "index_file.private_filesystem",
                              ObservationState::Unsupported);
    return Observation<bool>::available(true);
}
bool valid_io(std::uint64_t offset, std::size_t count) {
    const auto max = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    return offset <= max && count <= max - offset;
}
class PosixIndexFile final : public IndexByteFile {
  public:
    explicit PosixIndexFile(int fd) : fd_(fd) {
    }
    Observation<std::uint64_t> size() override {
        struct stat s{};
        if (fstat(fd_.value, &s) != 0)
            return failure<std::uint64_t>(errno, "index_file.fstat");
        if (!S_ISREG(s.st_mode) || s.st_size < 0 || s.st_nlink != 0)
            return rejected<std::uint64_t>(AdapterError::InvalidNativeResult, "index_file.stat");
        return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(s.st_size));
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        if (!valid_io(offset, bytes.size()))
            return rejected<std::size_t>(AdapterError::InvalidLimit, "index_file.write_range");
        const auto count = std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto n = pwrite(fd_.value, bytes.data(), count, static_cast<off_t>(offset));
        if (n < 0)
            return failure<std::size_t>(errno, "index_file.pwrite");
        return Observation<std::size_t>::available(static_cast<std::size_t>(n));
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) override {
        if (!valid_io(offset, bytes.size()))
            return rejected<std::size_t>(AdapterError::InvalidLimit, "index_file.read_range");
        const auto count = std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto n = pread(fd_.value, bytes.data(), count, static_cast<off_t>(offset));
        if (n < 0)
            return failure<std::size_t>(errno, "index_file.pread");
        return Observation<std::size_t>::available(static_cast<std::size_t>(n));
    }

  private:
    Fd fd_;
};
} // namespace
Observation<std::unique_ptr<IndexByteFile>> create_native_index_file(const NativeAbsolutePath &root) {
    using Pointer = std::unique_ptr<IndexByteFile>;
    auto host = root.host_path();
    if (!host.value()) {
        const auto &e = *host.unavailable_detail();
        return Observation<Pointer>::unavailable(e.state, e.error);
    }
    const auto &path = *host.value();
    if (path.native().back() == '/' || path.filename() == "." || path.filename() == "..")
        return rejected<Pointer>(AdapterError::InvalidPath, "index_file.root_address");
    Fd directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.value < 0)
        return failure<Pointer>(errno, "index_file.open_root");
    struct stat parent{};
    if (fstat(directory.value, &parent) != 0)
        return failure<Pointer>(errno, "index_file.root_stat");
    if (!S_ISDIR(parent.st_mode) || parent.st_uid != geteuid() || (parent.st_mode & 07777) != 0700)
        return rejected<Pointer>(AdapterError::InvalidAccess, "index_file.private_root", ObservationState::Denied);
    for (auto check : {qualified_volume(directory.value), private_acl(directory.value)}) {
        if (!check.value()) {
            const auto &e = *check.unavailable_detail();
            return Observation<Pointer>::unavailable(e.state, e.error);
        }
    }
    // All names below this held root are private to the trusted user. Create
    // exclusively and unlink BEFORE any data or caller-visible owner exists.
    // O_EXCL refuses preexisting links/files; no process umask/cwd modification.
    for (int attempt = 0; attempt != 64; ++attempt) {
        std::array<unsigned char, 16> entropy{};
        if (getentropy(entropy.data(), entropy.size()) != 0)
            return failure<Pointer>(errno, "index_file.getentropy");
        std::string name = "ck-files-index-";
        for (auto byte : entropy) {
            name += "0123456789abcdef"[byte >> 4];
            name += "0123456789abcdef"[byte & 15];
        }
        Fd file(openat(directory.value, name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (file.value < 0) {
            const int code = errno;
            if (code == EEXIST)
                continue;
            return failure<Pointer>(code, "index_file.openat_create");
        }
        if (unlinkat(directory.value, name.c_str(), 0) != 0)
            return failure<Pointer>(errno, "index_file.unlink_empty_cleanup_required");
        struct stat s{};
        if (fstat(file.value, &s) != 0)
            return failure<Pointer>(errno, "index_file.created_stat");
        if (!S_ISREG(s.st_mode) || s.st_uid != geteuid() || (s.st_mode & 07777 & ~0600) || s.st_nlink != 0 ||
            s.st_size != 0)
            return rejected<Pointer>(AdapterError::InvalidNativeResult, "index_file.created_private");
        auto acl = private_acl(file.value);
        if (!acl.value()) {
            const auto &e = *acl.unavailable_detail();
            return Observation<Pointer>::unavailable(e.state, e.error);
        }
        // Allocate before transferring the descriptor, including on bad_alloc.
        auto owner = std::make_unique<PosixIndexFile>(file.value);
        file.release();
        return Observation<Pointer>::available(std::move(owner));
    }
    return rejected<Pointer>(AdapterError::InventoryLimit, "index_file.name_attempts", ObservationState::Unknown);
}
} // namespace ck::files
