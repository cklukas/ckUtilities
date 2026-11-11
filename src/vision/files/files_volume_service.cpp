// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_service.hpp"
#include "native_filesystem.hpp"
#include "windows_name.hpp"
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cwchar>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <unistd.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#endif

namespace ck::vision {
namespace {
using ck::files::AdapterError;
using ck::files::ErrorDomain;
using ck::files::Observation;
using ck::files::ObservationState;
using ck::files::UnavailableObservation;

UnavailableObservation failure(const char *operation, ErrorDomain domain = ErrorDomain::Adapter, int code = 1) {
    return {ObservationState::Failed, {domain, code, operation}};
}

std::optional<std::uint64_t> filesystem_bytes(std::uint64_t blocks, std::uint64_t block_size) {
    if (!block_size || blocks > std::numeric_limits<std::uint64_t>::max() / block_size)
        return std::nullopt;
    return blocks * block_size;
}

#if defined(_WIN32)
std::string utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            bytes, nullptr, nullptr) != bytes)
        return {};
    return result;
}

void append_windows_volume(std::vector<FilesVolumeRecord> &result, std::wstring_view volume,
                           std::wstring_view mount) {
    wchar_t label[256]{};
    wchar_t filesystem[256]{};
    DWORD serial = 0, maximum_component = 0, flags = 0;
    const std::wstring volume_path(volume);
    const auto extended_volume = ck::files::windows_extended_path(volume_path);
    const bool info = extended_volume && GetVolumeInformationW(extended_volume->c_str(), label, static_cast<DWORD>(std::size(label)), &serial,
                                            &maximum_component, &flags, filesystem,
                                            static_cast<DWORD>(std::size(filesystem))) != 0;
    ULARGE_INTEGER free_bytes{}, total_bytes{}, available_bytes{};
    const std::wstring space_path = mount.empty() ? volume_path : std::wstring(mount);
    const auto extended_space = ck::files::windows_extended_path(space_path);
    const bool space = extended_space && GetDiskFreeSpaceExW(extended_space->c_str(), &available_bytes, &total_bytes, &free_bytes) != 0;
    FilesVolumeRecord record;
    record.name = info && label[0] ? utf8(label) : utf8(volume);
    record.mount_point = mount.empty() ? utf8(volume) : utf8(mount);
    record.filesystem = info && filesystem[0] ? utf8(filesystem) : "Unknown";
    record.identity = utf8(volume);
    const auto native_mount = mount.empty() ? volume : mount;
    record.native_mount_units.assign(reinterpret_cast<const char16_t *>(native_mount.data()), native_mount.size());
    if (space) {
        record.total_bytes = total_bytes.QuadPart;
        record.free_bytes = available_bytes.QuadPart;
    }
    record.read_only = info && (flags & FILE_READ_ONLY_VOLUME) != 0;
    // The inventory uses UTF-8 only for display. Archive traversal retains
    // exact UTF-16 units and qualifies them with a native read-only probe.
    auto address = ck::files::NativeAbsolutePath::from_windows(record.native_mount_units);
    if (!address.value()) {
        record.archive.support = FilesVolumeArchiveSupport::Unsupported;
        record.archive.diagnostic = "Native mount path is not representable";
    } else {
        ck::files::NativeFileSystem filesystem_adapter;
        auto opened = filesystem_adapter.open_native_directory(*address.value());
        if (!opened.value()) {
            const auto *detail = opened.unavailable_detail();
            record.archive.support = detail && detail->state == ObservationState::Denied
                                         ? FilesVolumeArchiveSupport::Denied
                                     : detail && detail->state == ObservationState::Unsupported
                                         ? FilesVolumeArchiveSupport::Unsupported
                                         : FilesVolumeArchiveSupport::Unknown;
            record.archive.diagnostic = detail ? "Archive capability probe failed: " + detail->error.operation
                                               : "Archive capability probe failed";
        } else {
            auto metadata = (*opened.value())->inspect_basic();
            if (!metadata.value()) {
                const auto *detail = metadata.unavailable_detail();
                record.archive.support = detail && detail->state == ObservationState::Denied
                                             ? FilesVolumeArchiveSupport::Denied
                                         : detail && detail->state == ObservationState::Unsupported
                                             ? FilesVolumeArchiveSupport::Unsupported
                                             : FilesVolumeArchiveSupport::Unknown;
                record.archive.diagnostic = detail ? "Archive capability probe failed: " + detail->error.operation
                                                   : "Archive capability probe failed";
            } else if (!metadata.value()->kind.value() ||
                       *metadata.value()->kind.value() != ck::files::ObjectKind::Directory ||
                       !metadata.value()->identity.value() || !metadata.value()->mount_id.value()) {
                record.archive.support = FilesVolumeArchiveSupport::Unknown;
                record.archive.diagnostic = "Archive capability probe could not qualify the mount identity";
            } else {
                const auto volume_address = ck::files::NativeAbsolutePath::from_windows(
                    std::u16string_view(reinterpret_cast<const char16_t *>(volume.data()), volume.size()));
                if (volume_address.value()) {
                    const auto volume_root = filesystem_adapter.open_native_directory(*volume_address.value());
                    if (volume_root.value() && (*volume_root.value())->identity().value() &&
                        ck::files::compare_identity(*metadata.value()->identity.value(),
                            *(*volume_root.value())->identity().value()) == ck::files::IdentityComparison::Same)
                        record.archive.root = {*metadata.value()->identity.value(), *metadata.value()->mount_id.value()};
                }
                if (record.archive.root) {
                    record.archive.support = FilesVolumeArchiveSupport::LiveReadable;
                    record.archive.diagnostic = "Live source traversal qualified; snapshot provider not configured";
                } else {
                    record.archive.diagnostic = "Mount path could not be bound to its volume root";
                }
            }
        }
    }
    result.push_back(std::move(record));
}

Observation<std::vector<FilesVolumeRecord>> enumerate_windows() {
    std::vector<FilesVolumeRecord> result;
    // FindFirstVolumeW returns a volume GUID name, including its trailing
    // separator. This buffer holds that format, independently of path limits.
    wchar_t volume_name[64]{};
    HANDLE handle = FindFirstVolumeW(volume_name, static_cast<DWORD>(std::size(volume_name)));
    if (handle == INVALID_HANDLE_VALUE)
        return Observation<std::vector<FilesVolumeRecord>>::unavailable(ObservationState::Failed,
                                                                          failure("volumes.windows.first", ErrorDomain::Win32,
                                                                                  static_cast<int>(GetLastError()))
                                                                              .error);
    do {
        DWORD required = 0;
        std::vector<wchar_t> paths(512);
        if (GetVolumePathNamesForVolumeNameW(volume_name, paths.data(), static_cast<DWORD>(paths.size()), &required)) {
            const wchar_t *current = paths.data();
            while (*current) {
                append_windows_volume(result, volume_name, current);
                current += std::wcslen(current) + 1;
            }
        } else if (GetLastError() == ERROR_MORE_DATA && required > paths.size()) {
            paths.resize(required);
            if (GetVolumePathNamesForVolumeNameW(volume_name, paths.data(), static_cast<DWORD>(paths.size()), &required)) {
                const wchar_t *current = paths.data();
                while (*current) {
                    append_windows_volume(result, volume_name, current);
                    current += std::wcslen(current) + 1;
                }
            }
        } else {
            append_windows_volume(result, volume_name, {});
        }
    } while (FindNextVolumeW(handle, volume_name, static_cast<DWORD>(std::size(volume_name))));
    FindVolumeClose(handle);
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.mount_point < right.mount_point;
    });
    if (result.empty())
        return Observation<std::vector<FilesVolumeRecord>>::unavailable(ObservationState::Unknown,
                                                                          failure("volumes.windows.empty").error);
    return Observation<std::vector<FilesVolumeRecord>>::available(std::move(result));
}
#else
#if !defined(__APPLE__)
std::string decode_mountinfo(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3 < value.size() && value[i + 1] >= '0' && value[i + 1] <= '7' &&
            value[i + 2] >= '0' && value[i + 2] <= '7' && value[i + 3] >= '0' && value[i + 3] <= '7') {
            result.push_back(static_cast<char>((value[i + 1] - '0') * 64 + (value[i + 2] - '0') * 8 + value[i + 3] - '0'));
            i += 3;
        } else
            result.push_back(value[i]);
    }
    return result;
}

void append_posix_volume(std::vector<FilesVolumeRecord> &result, std::string name, std::string mount,
                         std::string filesystem, bool read_only, std::string identity) {
    struct statvfs stats{};
    FilesVolumeRecord record{std::move(name), std::move(mount), std::move(filesystem), {}, {}, read_only, {}, {}, {}};
    record.identity = std::move(identity);
    record.archive.diagnostic = "Archive capability probe not run";
    if (statvfs(record.mount_point.c_str(), &stats) == 0) {
        record.total_bytes = filesystem_bytes(stats.f_blocks, stats.f_frsize);
        record.free_bytes = filesystem_bytes(stats.f_bavail, stats.f_frsize);
    }
    result.push_back(std::move(record));
}
#endif

void probe_posix_archive_capability(FilesVolumeRecord &record,
#if defined(__APPLE__)
                                    const ck::files::NativeIdentifier &filesystem_id
#else
                                    std::uint64_t mount_id
#endif
) {
    auto address = ck::files::NativeAbsolutePath::from_posix(record.mount_point);
    if (!address.value()) {
        record.archive.support = FilesVolumeArchiveSupport::Unsupported;
        record.archive.diagnostic = "Native mount path is not representable";
        return;
    }
    ck::files::NativeFileSystem filesystem;
    auto opened = filesystem.open_native_directory(*address.value());
    if (!opened.value()) {
        const auto *detail = opened.unavailable_detail();
        if (detail && detail->state == ObservationState::Denied)
            record.archive.support = FilesVolumeArchiveSupport::Denied;
        else if (detail && detail->state == ObservationState::Unsupported)
            record.archive.support = FilesVolumeArchiveSupport::Unsupported;
        else
            record.archive.support = FilesVolumeArchiveSupport::Unknown;
        record.archive.diagnostic = detail ? "Archive capability probe failed: " + detail->error.operation
                                           : "Archive capability probe failed";
        return;
    }
    auto metadata = (*opened.value())->inspect_basic();
    if (!metadata.value()) {
        const auto *detail = metadata.unavailable_detail();
        record.archive.support = detail && detail->state == ObservationState::Denied
                                     ? FilesVolumeArchiveSupport::Denied
                                 : detail && detail->state == ObservationState::Unsupported
                                     ? FilesVolumeArchiveSupport::Unsupported
                                     : FilesVolumeArchiveSupport::Unknown;
        record.archive.diagnostic = detail ? "Archive capability probe failed: " + detail->error.operation
                                           : "Archive capability probe failed";
        return;
    }
    if (!metadata.value()->kind.value() ||
        *metadata.value()->kind.value() != ck::files::ObjectKind::Directory ||
        !metadata.value()->identity.value() || !metadata.value()->mount_id.value()) {
        record.archive.support = FilesVolumeArchiveSupport::Unknown;
        record.archive.diagnostic = "Archive capability probe could not qualify the mount identity";
        return;
    }
#if defined(__APPLE__)
    const auto location = (*opened.value())->inspect_location();
    const bool matches = location.value() && location.value()->filesystem_query_id.value() &&
        *location.value()->filesystem_query_id.value() == filesystem_id;
#else
    const bool matches = *metadata.value()->mount_id.value() == mount_id;
#endif
    if (!matches) {
        record.archive.diagnostic = "Mount path could not be bound to its volume root";
        return;
    }
    record.archive.root = {*metadata.value()->identity.value(), *metadata.value()->mount_id.value()};
    // This is only a source-read eligibility probe. The worker still performs
    // the complete object inventory and the review/writer rejects any hard
    // loss, unsupported object, or source change.
    record.archive.support = FilesVolumeArchiveSupport::LiveReadable;
    record.archive.snapshot_available = false;
    record.archive.diagnostic = "Live source traversal qualified; snapshot provider not configured";
}

#if defined(__APPLE__)
Observation<std::vector<FilesVolumeRecord>> enumerate_macos() {
    std::vector<struct statfs> snapshot;
    {
        // getmntinfo returns shared storage. Copy it before another Files
        // worker can request the inventory, then release the lock before I/O.
        static std::mutex inventory_mutex;
        const std::lock_guard lock(inventory_mutex);
        struct statfs *mounts = nullptr;
        const int count = getmntinfo(&mounts, MNT_NOWAIT);
        if (count <= 0)
            return Observation<std::vector<FilesVolumeRecord>>::unavailable(ObservationState::Failed,
                failure("volumes.macos.getmntinfo", ErrorDomain::Posix, errno).error);
        snapshot.assign(mounts, mounts + count);
    }
    std::vector<FilesVolumeRecord> result;
    std::set<std::string> seen;
    for (const auto &entry : snapshot) {
        const std::string mount = entry.f_mntonname;
        if (!seen.insert(mount).second)
            continue;
        // A device path or volume label can be reused after an unmount. Keep
        // the filesystem identity and source only as an opaque revalidation
        // token; the archive worker compares this token again before reading.
        std::string identity =
            "macos:" + std::to_string(static_cast<std::int64_t>(entry.f_fsid.val[0])) + ":" +
            std::to_string(static_cast<std::int64_t>(entry.f_fsid.val[1])) + ":" +
            std::string(entry.f_mntfromname);
        FilesVolumeRecord record{entry.f_mntfromname, mount, entry.f_fstypename, {}, {},
                                 (entry.f_flags & MNT_RDONLY) != 0, std::move(identity), {}, {}};
        // Darwin statvfs can truncate the APFS total block count above UINT32_MAX.
        struct statfs stats{};
        if (statfs(mount.c_str(), &stats) == 0 && mount == stats.f_mntonname &&
            std::memcmp(&stats.f_fsid, &entry.f_fsid, sizeof(entry.f_fsid)) == 0) {
            record.total_bytes = filesystem_bytes(stats.f_blocks, stats.f_bsize);
            record.free_bytes = filesystem_bytes(stats.f_bavail, stats.f_bsize);
        }
        ck::files::NativeIdentifier filesystem_id;
        static_assert(sizeof(entry.f_fsid) <= sizeof(filesystem_id.bytes));
        std::memcpy(filesystem_id.bytes.data(), &entry.f_fsid, sizeof(entry.f_fsid));
        probe_posix_archive_capability(record, filesystem_id);
        result.push_back(std::move(record));
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.mount_point < right.mount_point;
    });
    return Observation<std::vector<FilesVolumeRecord>>::available(std::move(result));
}
#else
Observation<std::vector<FilesVolumeRecord>> enumerate_linux() {
    std::ifstream input("/proc/self/mountinfo");
    if (!input)
        return Observation<std::vector<FilesVolumeRecord>>::unavailable(ObservationState::Denied,
                                                                          failure("volumes.linux.mountinfo", ErrorDomain::Posix,
                                                                                  errno)
                                                                              .error);
    std::vector<FilesVolumeRecord> result;
    std::set<std::string> seen;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string token, mount_point, options;
        bool separator = false;
        std::vector<std::string> before_separator;
        while (stream >> token) {
            if (token == "-") {
                separator = true;
                break;
            }
            before_separator.push_back(token);
        }
        std::string filesystem, source, super_options;
        if (separator)
            stream >> filesystem >> source >> super_options;
        if (before_separator.size() < 6 || filesystem.empty())
            continue;
        mount_point = decode_mountinfo(before_separator[4]);
        options = before_separator[5];
        if (!seen.insert(mount_point).second)
            continue;
        const bool read_only = options == "ro" || options.rfind("ro,", 0) == 0 ||
                               (options.size() > 3 && options.ends_with(",ro")) ||
                               options.find(",ro,") != std::string::npos;
        const auto decoded_source = decode_mountinfo(source);
        // mountinfo's mount ID changes when the mount instance is replaced,
        // while major:minor distinguishes the backing device. Neither a
        // display path nor the source string alone is sufficient for archive
        // revalidation after an unmount/remount cycle.
        const std::string identity = "linux:" + before_separator[0] + ":" + before_separator[2] + ":" +
                                     decoded_source;
        append_posix_volume(result, decoded_source, mount_point, decode_mountinfo(filesystem), read_only,
                             identity);
        const auto &mount_id = before_separator[0];
        std::uint64_t native_mount_id = 0;
        const auto parsed = std::from_chars(mount_id.data(), mount_id.data() + mount_id.size(), native_mount_id);
        if (parsed.ec == std::errc{} && parsed.ptr == mount_id.data() + mount_id.size())
            probe_posix_archive_capability(result.back(), native_mount_id);
    }
    if (result.empty())
        return Observation<std::vector<FilesVolumeRecord>>::unavailable(ObservationState::Unknown,
                                                                          failure("volumes.linux.empty").error);
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.mount_point < right.mount_point;
    });
    return Observation<std::vector<FilesVolumeRecord>>::available(std::move(result));
}
#endif
#endif
} // namespace

ck::files::Observation<std::vector<FilesVolumeRecord>> enumerate_native_volumes() {
#if defined(_WIN32)
    return enumerate_windows();
#elif defined(__APPLE__)
    return enumerate_macos();
#else
    return enumerate_linux();
#endif
}

struct FilesVolumeService::State {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false, requested = false, working = false;
    std::optional<FilesVolumeReply> reply;
    std::function<void()> wake;
    FilesVolumeEnumerator enumerator;
    std::thread worker;

    State(std::function<void()> signal, FilesVolumeEnumerator source)
        : wake(std::move(signal)), enumerator(source ? std::move(source) : enumerate_native_volumes),
          worker([this] { run(); }) {}

    void run() {
        for (;;) {
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || requested; });
                if (stopping)
                    return;
                requested = false;
                working = true;
            }
            FilesVolumeReply value;
            try {
                auto result = enumerator();
                if (result.value()) {
                    value.state = FilesVolumeReplyState::Ready;
                    value.volumes = std::move(result).take_value();
                } else {
                    value.state = FilesVolumeReplyState::Failed;
                    value.error = *result.unavailable_detail();
                }
            } catch (const std::bad_alloc &) {
                value.state = FilesVolumeReplyState::WorkerFault;
                value.error = failure("volumes.worker.allocation");
            } catch (...) {
                value.state = FilesVolumeReplyState::WorkerFault;
                value.error = failure("volumes.worker.exception");
            }
            std::function<void()> signal;
            {
                std::lock_guard lock(mutex);
                working = false;
                reply = std::move(value);
                signal = wake;
            }
            if (signal) {
                try {
                    signal();
                } catch (...) {
                }
            }
        }
    }
};

FilesVolumeService::FilesVolumeService(std::function<void()> wake, FilesVolumeEnumerator enumerator)
    : state_(std::make_unique<State>(std::move(wake), std::move(enumerator))) {}
FilesVolumeService::~FilesVolumeService() { join(); }
bool FilesVolumeService::request() {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->requested || state_->working || state_->reply)
        return false;
    state_->requested = true;
    state_->changed.notify_one();
    return true;
}
std::optional<FilesVolumeReply> FilesVolumeService::take() {
    std::lock_guard lock(state_->mutex);
    if (!state_->reply)
        return std::nullopt;
    auto value = std::move(state_->reply);
    state_->reply.reset();
    return value;
}
void FilesVolumeService::request_stop() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->requested = false;
    state_->changed.notify_all();
}
void FilesVolumeService::join() {
    request_stop();
    if (state_->worker.joinable()) {
        if (state_->worker.get_id() == std::this_thread::get_id())
            throw std::logic_error("volume service cannot join itself");
        state_->worker.join();
    }
}
bool FilesVolumeService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
} // namespace ck::vision
