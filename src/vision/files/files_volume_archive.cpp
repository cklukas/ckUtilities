// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive.hpp"
#include "native_filesystem.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <tuple>
#include <utility>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ck::vision {
namespace {
using ck::files::AdapterError;
using ck::files::ErrorDomain;
using ck::files::Observation;
using ck::files::ObservationState;
using ck::files::UnavailableObservation;

constexpr std::array<char, 8> kMagic{'C', 'K', 'A', 'R', 'V', '0', '1', '\0'};
constexpr std::uint32_t kVersion = 2;
constexpr std::uint64_t kHeaderSize = 56;

struct ArchiveDiagnostic final {
    ObservationState state = ObservationState::Failed;
    ck::files::NativeError error;
    operator ck::files::NativeError() const { return error; }
    operator UnavailableObservation() const { return {state, error}; }
};

ArchiveDiagnostic error(AdapterError code, const char *operation,
                         ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

UnavailableObservation filesystem_error(const char *operation, int code) {
#if defined(_WIN32)
    return {ObservationState::Failed, {ErrorDomain::Win32, code, operation}};
#else
    return {ObservationState::Failed, {ErrorDomain::Posix, code, operation}};
#endif
}

template <class T>
void append_le(std::vector<std::byte> &out, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t i = 0; i != sizeof(T); ++i)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
}

void append_string(std::vector<std::byte> &out, std::string_view value) {
    append_le<std::uint64_t>(out, value.size());
    for (const unsigned char byte : value)
        out.push_back(std::byte{byte});
}

std::optional<std::string> native_path_bytes(const ck::files::NativeAbsolutePath &path) {
    if (path.encoding() == ck::files::NameEncoding::PosixBytes) {
        if (const auto *bytes = path.posix_bytes())
            return *bytes;
    } else if (path.encoding() == ck::files::NameEncoding::WindowsUtf16) {
        if (const auto *units = path.windows_units()) {
            std::string result;
            result.reserve(units->size() * 2);
            for (const auto unit : *units) {
                result.push_back(static_cast<char>(unit & 0xff));
                result.push_back(static_cast<char>(unit >> 8));
            }
            return result;
        }
    }
    return std::nullopt;
}

void append_native_path_evidence(std::vector<std::byte> &out, const ck::files::NativeAbsolutePath &path) {
    append_le<std::uint8_t>(out, static_cast<std::uint8_t>(path.encoding()));
    append_string(out, *native_path_bytes(path));
}

bool safe_archive_name(std::string_view name);

constexpr std::string_view archive_journal_prefix = ".ck-volume-archive-";

bool is_archive_journal_name(std::string_view value) {
    if (!value.starts_with(archive_journal_prefix) ||
        value.size() != archive_journal_prefix.size() + 32)
        return false;
    const auto suffix = value.substr(archive_journal_prefix.size());
    return std::all_of(suffix.begin(), suffix.end(), [](unsigned char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

std::optional<ck::files::NativeName> native_journal_name(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto units = path.filename().wstring();
    return ck::files::NativeName::from_windows(
        std::u16string(reinterpret_cast<const char16_t *>(units.data()), units.size()));
#else
    return ck::files::NativeName::from_posix(path.filename().string());
#endif
}

template <class T>
bool read_le(std::span<const std::byte> bytes, std::size_t &offset, T &value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        return false;
    value = 0;
    for (std::size_t index = 0; index != sizeof(T); ++index)
        value |= static_cast<T>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
    offset += sizeof(T);
    return true;
}

bool read_string(std::span<const std::byte> bytes, std::size_t &offset, std::string &value) {
    std::uint64_t size = 0;
    constexpr std::uint64_t max_evidence_string = 4096;
    if (!read_le(bytes, offset, size) || size > max_evidence_string || size > bytes.size() - offset)
        return false;
    value.assign(reinterpret_cast<const char *>(bytes.data() + offset), static_cast<std::size_t>(size));
    offset += static_cast<std::size_t>(size);
    return true;
}

constexpr std::uint8_t kStagingPathEvidence = 1U;
constexpr std::uint8_t kStagingDigestEvidence = 2U;

Observation<ck::files::NativeAbsolutePath> decode_native_absolute_path(
    ck::files::NameEncoding encoding, std::string_view bytes) {
    if (encoding == ck::files::NameEncoding::PosixBytes)
        return ck::files::NativeAbsolutePath::from_posix(bytes);
    if (encoding == ck::files::NameEncoding::WindowsUtf16) {
        if (bytes.size() % 2 != 0)
            return Observation<ck::files::NativeAbsolutePath>::unavailable(
                ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_utf16_path"));
        std::u16string units;
        units.reserve(bytes.size() / 2);
        for (std::size_t index = 0; index != bytes.size(); index += 2)
            units.push_back(static_cast<char16_t>(static_cast<unsigned char>(bytes[index]) |
                                                  static_cast<unsigned char>(bytes[index + 1]) << 8));
        return ck::files::NativeAbsolutePath::from_windows(units);
    }
    return Observation<ck::files::NativeAbsolutePath>::unavailable(
        ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_path_encoding"));
}

bool read_native_absolute_path(std::span<const std::byte> bytes, std::size_t &offset,
                               std::optional<ck::files::NameEncoding> &encoding, std::string &path) {
    std::uint8_t value = 0;
    if (!read_le(bytes, offset, value) ||
        (value != static_cast<std::uint8_t>(ck::files::NameEncoding::PosixBytes) &&
         value != static_cast<std::uint8_t>(ck::files::NameEncoding::WindowsUtf16)) ||
        !read_string(bytes, offset, path) || path.empty())
        return false;
    encoding = static_cast<ck::files::NameEncoding>(value);
    return decode_native_absolute_path(*encoding, path).value();
}

Observation<FilesVolumeArchiveJournalEvidence> decode_archive_journal_evidence(
    std::span<const std::byte> bytes) {
    FilesVolumeArchiveJournalEvidence result;
    std::size_t offset = 0;
    if (!read_le(bytes, offset, result.transaction_id) || result.transaction_id == 0 ||
        !read_string(bytes, offset, result.source_identity) || result.source_identity.empty() ||
        !read_string(bytes, offset, result.source_mount_point) || result.source_mount_point.empty() ||
        !read_string(bytes, offset, result.archive_name) || !safe_archive_name(result.archive_name))
        return Observation<FilesVolumeArchiveJournalEvidence>::unavailable(
            ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_evidence"));
    if (offset != bytes.size()) {
        if (!read_native_absolute_path(bytes, offset, result.destination_encoding, result.destination_native_path))
            return Observation<FilesVolumeArchiveJournalEvidence>::unavailable(
                ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_destination"));
        if (offset == bytes.size())
            return Observation<FilesVolumeArchiveJournalEvidence>::available(std::move(result));
        std::uint8_t flags = 0;
        if (!read_le(bytes, offset, flags) || (flags & ~(kStagingPathEvidence | kStagingDigestEvidence)) != 0 ||
            ((flags & kStagingDigestEvidence) != 0 && (flags & kStagingPathEvidence) == 0))
            return Observation<FilesVolumeArchiveJournalEvidence>::unavailable(
                ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_evidence_flags"));
        if (flags & kStagingPathEvidence) {
            if (!read_native_absolute_path(bytes, offset, result.staging_encoding, result.staging_native_path))
                return Observation<FilesVolumeArchiveJournalEvidence>::unavailable(
                    ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_staging"));
        }
        if (flags & kStagingDigestEvidence) {
            if (bytes.size() - offset < 32)
                return Observation<FilesVolumeArchiveJournalEvidence>::unavailable(
                    ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_digest"));
            std::array<std::byte, 32> digest{};
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), digest.size(), digest.begin());
            offset += digest.size();
            result.staging_digest = digest;
        }
        if (offset != bytes.size())
            return Observation<FilesVolumeArchiveJournalEvidence>::unavailable(
                ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_evidence_tail"));
    }
    return Observation<FilesVolumeArchiveJournalEvidence>::available(std::move(result));
}

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t &result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

bool safe_relative_path(std::string_view path, ck::files::NameEncoding encoding, bool allow_root = false) {
    if (encoding == ck::files::NameEncoding::PosixBytes) {
        if (allow_root && path == ".")
            return true;
        if (path.empty() || path.front() == '/' || path.find('\0') != std::string_view::npos)
            return false;
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto end = path.find('/', start);
            const auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
            if (component.empty() || component == "." || component == "..")
                return false;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return true;
    }
    if (encoding != ck::files::NameEncoding::WindowsUtf16 || path.empty() || path.size() % 2 != 0)
        return false;
    auto unit = [&](std::size_t index) {
        return static_cast<char16_t>(static_cast<unsigned char>(path[index * 2])) |
               static_cast<char16_t>(static_cast<unsigned char>(path[index * 2 + 1])) << 8;
    };
    const auto units = path.size() / 2;
    if (allow_root && units == 1 && unit(0) == u'.')
        return true;
    if (units == 0 || unit(0) == u'\\' || unit(0) == u'/')
        return false;
    std::size_t start = 0;
    while (start <= units) {
        std::size_t end = start;
        while (end < units && unit(end) != u'\\')
            ++end;
        if (end == start)
            return false;
        if (end - start == 1 && unit(start) == u'.')
            return false;
        if (end - start == 2 && unit(start) == u'.' && unit(start + 1) == u'.')
            return false;
        if (end == units)
            break;
        start = end + 1;
    }
    return true;
}

bool safe_archive_name(std::string_view name) {
    return !name.empty() && name.find_first_of("/\\") == std::string_view::npos &&
           name != "." && name != ".." && name.find('\0') == std::string_view::npos &&
           name.size() >= std::string_view{".ckarchive"}.size() &&
           name.ends_with(".ckarchive");
}

struct EncodedExtent final {
    std::uint64_t source_offset = 0;
    std::uint64_t archive_offset = 0;
    std::uint64_t length = 0;
    const std::vector<std::byte> *bytes = nullptr;
};

struct EncodedEntry final {
    const FilesVolumeArchiveEntry *source = nullptr;
    std::vector<EncodedExtent> extents;
};

Observation<std::vector<EncodedEntry>> validate_and_encode(const FilesVolumeArchiveDocument &document,
                                                            std::uint64_t &data_size,
                                                            std::uint64_t &data_bytes,
                                                            std::stop_token stop_token) {
    if (document.source.identity.empty() || document.source.mount_point.empty())
        return Observation<std::vector<EncodedEntry>>::unavailable(
            ObservationState::Unknown, error(AdapterError::UnqualifiedIdentity, "volume_archive.source_identity"));
    if (document.consistency == FilesVolumeArchiveConsistency::Live &&
        document.source.archive.support != FilesVolumeArchiveSupport::LiveReadable &&
        document.source.archive.support != FilesVolumeArchiveSupport::SnapshotPreferred)
        return Observation<std::vector<EncodedEntry>>::unavailable(
            ObservationState::Unknown, error(AdapterError::ProviderReviewRequired, "volume_archive.live_capability"));
    if (document.consistency == FilesVolumeArchiveConsistency::Snapshot &&
        document.source.archive.support != FilesVolumeArchiveSupport::SnapshotPreferred)
        return Observation<std::vector<EncodedEntry>>::unavailable(
            ObservationState::Unsupported, error(AdapterError::ProviderReviewRequired, "volume_archive.snapshot_capability"));
    if (document.entries.empty())
        return Observation<std::vector<EncodedEntry>>::unavailable(
            ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.empty_document"));
    if (document.path_encoding != ck::files::NameEncoding::PosixBytes &&
        document.path_encoding != ck::files::NameEncoding::WindowsUtf16)
        return Observation<std::vector<EncodedEntry>>::unavailable(
            ObservationState::Unsupported, error(AdapterError::UnrepresentableName, "volume_archive.path_encoding"));
    for (const auto &loss : document.losses) {
        if (loss.entry_index >= document.entries.size() ||
            (!loss.path.empty() && !safe_relative_path(loss.path, document.path_encoding, true)) ||
            loss.diagnostic.empty())
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unsupported, error(AdapterError::PlanInvalid, "volume_archive.loss_record"));
    }

    std::set<std::string> paths;
    std::vector<EncodedEntry> encoded;
    encoded.reserve(document.entries.size());
    data_size = data_bytes = 0;
    for (const auto &entry : document.entries) {
        if (stop_token.stop_requested())
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InterruptedRead, "volume_archive.cancelled"));
        if (entry.kind < FilesVolumeArchiveEntryKind::Directory ||
            entry.kind > FilesVolumeArchiveEntryKind::Unsupported)
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unsupported, error(AdapterError::PlanInvalid, "volume_archive.entry_kind"));
        const bool root_entry = entry.kind == FilesVolumeArchiveEntryKind::Directory &&
            ((document.path_encoding == ck::files::NameEncoding::PosixBytes && entry.path == ".") ||
             (document.path_encoding == ck::files::NameEncoding::WindowsUtf16 && entry.path.size() == 2 &&
              entry.path[0] == '.' && entry.path[1] == '\0'));
        if (!safe_relative_path(entry.path, document.path_encoding, root_entry) || !paths.insert(entry.path).second)
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unsupported, error(AdapterError::UnrepresentableName, "volume_archive.entry_path"));
        if (entry.kind == FilesVolumeArchiveEntryKind::HardLink && entry.hard_link_group == 0)
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InvalidIdentifier, "volume_archive.hard_link"));
        if (entry.kind != FilesVolumeArchiveEntryKind::RegularFile &&
            entry.kind != FilesVolumeArchiveEntryKind::Unsupported &&
            (entry.logical_size != 0 || !entry.extents.empty()))
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unsupported, error(AdapterError::PlanInvalid, "volume_archive.non_file_data"));

        EncodedEntry value;
        value.source = &entry;
        std::uint64_t previous_end = 0;
        for (const auto &extent : entry.extents) {
            if (extent.bytes.empty() || extent.offset < previous_end || extent.offset > entry.logical_size)
                return Observation<std::vector<EncodedEntry>>::unavailable(
                    ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.extent_order"));
            if (extent.bytes.size() > entry.logical_size - extent.offset)
                return Observation<std::vector<EncodedEntry>>::unavailable(
                    ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.extent_bounds"));
            std::uint64_t end = 0;
            if (!checked_add(extent.offset, extent.bytes.size(), end))
                return Observation<std::vector<EncodedEntry>>::unavailable(
                    ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.extent_overflow"));
            previous_end = end;
            std::uint64_t next_data_bytes = 0;
            if (!checked_add(data_bytes, extent.bytes.size(), next_data_bytes))
                return Observation<std::vector<EncodedEntry>>::unavailable(
                    ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.data_bytes"));
            data_bytes = next_data_bytes;
        }
        std::uint64_t next_data_size = 0;
        if (!checked_add(data_size, entry.logical_size, next_data_size))
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.logical_size"));
        for (const auto &extent : entry.extents) {
            std::uint64_t archive_offset = 0;
            if (!checked_add(data_size, extent.offset, archive_offset))
                return Observation<std::vector<EncodedEntry>>::unavailable(
                    ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.archive_offset"));
            value.extents.push_back({extent.offset, archive_offset, extent.bytes.size(), &extent.bytes});
        }
        data_size = next_data_size;
        encoded.push_back(std::move(value));
    }
    return Observation<std::vector<EncodedEntry>>::available(std::move(encoded));
}

void append_header(std::vector<std::byte> &out, std::uint32_t flags, std::uint64_t manifest_size,
                   std::uint64_t data_offset, std::uint64_t data_size, std::uint64_t entry_count) {
    for (const char byte : kMagic)
        out.push_back(std::byte{static_cast<unsigned char>(byte)});
    append_le<std::uint32_t>(out, kVersion);
    append_le<std::uint32_t>(out, flags);
    append_le<std::uint64_t>(out, manifest_size);
    append_le<std::uint64_t>(out, data_offset);
    append_le<std::uint64_t>(out, data_size);
    append_le<std::uint64_t>(out, entry_count);
    append_le<std::uint64_t>(out, 0);
}

void append_manifest(std::vector<std::byte> &out, const FilesVolumeArchiveDocument &document,
                     const std::vector<EncodedEntry> &entries) {
    append_string(out, document.source.identity);
    append_string(out, document.source.name);
    append_string(out, document.source.mount_point);
    append_string(out, document.source.filesystem);
    append_le<std::uint8_t>(out, static_cast<std::uint8_t>(document.consistency));
    append_le<std::uint8_t>(out, static_cast<std::uint8_t>(document.path_encoding));
    for (int i = 0; i != 6; ++i)
        append_le<std::uint8_t>(out, 0);
    for (const auto &encoded : entries) {
        const auto &entry = *encoded.source;
        append_le<std::uint8_t>(out, static_cast<std::uint8_t>(entry.kind));
        for (int i = 0; i != 7; ++i)
            append_le<std::uint8_t>(out, 0);
        append_le<std::uint64_t>(out, entry.logical_size);
        append_le<std::uint64_t>(out, entry.hard_link_group);
        append_string(out, entry.path);
        append_string(out, entry.link_target);
        append_le<std::uint64_t>(out, encoded.extents.size());
        for (const auto &extent : encoded.extents) {
            append_le<std::uint64_t>(out, extent.source_offset);
            append_le<std::uint64_t>(out, extent.archive_offset);
            append_le<std::uint64_t>(out, extent.length);
        }
    }
    append_le<std::uint64_t>(out, document.losses.size());
    for (const auto &loss : document.losses) {
        append_le<std::uint64_t>(out, loss.entry_index);
        append_le<std::uint8_t>(out, static_cast<std::uint8_t>(loss.kind));
        append_le<std::uint8_t>(out, loss.hard_failure ? 1U : 0U);
        for (int i = 0; i != 6; ++i)
            append_le<std::uint8_t>(out, 0);
        append_string(out, loss.path);
        append_string(out, loss.diagnostic);
    }
}

bool create_exclusive(const std::filesystem::path &path, UnavailableObservation &failure_detail) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failure_detail = filesystem_error("volume_archive.create_temp", static_cast<int>(GetLastError()));
        return false;
    }
    CloseHandle(handle);
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        failure_detail = filesystem_error("volume_archive.create_temp", errno);
        return false;
    }
    ::close(fd);
    return true;
#endif
}

bool enable_sparse(const std::filesystem::path &path, UnavailableObservation &failure_detail) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failure_detail = filesystem_error("volume_archive.open_sparse", static_cast<int>(GetLastError()));
        return false;
    }
    DWORD returned = 0;
    const bool success = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
    if (!success)
        failure_detail = filesystem_error("volume_archive.set_sparse", static_cast<int>(GetLastError()));
    CloseHandle(handle);
    return success;
#else
    (void)path;
    (void)failure_detail;
    return true;
#endif
}

bool durable_flush(const std::filesystem::path &path, UnavailableObservation &failure_detail) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failure_detail = filesystem_error("volume_archive.flush_file", static_cast<int>(GetLastError()));
        return false;
    }
    const bool success = FlushFileBuffers(handle) != 0;
    if (!success)
        failure_detail = filesystem_error("volume_archive.flush_file", static_cast<int>(GetLastError()));
    CloseHandle(handle);
    return success;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        failure_detail = filesystem_error("volume_archive.open_flush", errno);
        return false;
    }
    const bool success = ::fsync(fd) == 0;
    if (!success)
        failure_detail = filesystem_error("volume_archive.flush_file", errno);
    ::close(fd);
    return success;
#endif
}

Observation<std::array<std::byte, 32>> digest_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return Observation<std::array<std::byte, 32>>::unavailable(
            ObservationState::Denied, filesystem_error("volume_archive.open_digest", errno).error);
    std::array<char, 64 * 1024> buffer{};
    ck::files::Sha256 digest;
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0)
            digest.update(std::as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(count))));
        if (stream.bad())
            return Observation<std::array<std::byte, 32>>::unavailable(
                ObservationState::Failed, filesystem_error("volume_archive.read_digest", errno).error);
    }
    return Observation<std::array<std::byte, 32>>::available(digest.digest());
}

bool durable_directory(const std::filesystem::path &path, UnavailableObservation &failure_detail) {
#if defined(_WIN32)
    (void)path;
    (void)failure_detail;
    return true;
#else
#ifdef O_DIRECTORY
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
#endif
    if (fd < 0) {
        failure_detail = filesystem_error("volume_archive.open_directory", errno);
        return false;
    }
    const bool success = ::fsync(fd) == 0;
    if (!success)
        failure_detail = filesystem_error("volume_archive.flush_directory", errno);
    ::close(fd);
    return success;
#endif
}

bool publish_without_replace(const std::filesystem::path &temporary, const std::filesystem::path &published,
                             UnavailableObservation &failure_detail, bool &published_result) {
    published_result = false;
    std::error_code ec;
    std::filesystem::create_hard_link(temporary, published, ec);
    if (ec) {
        failure_detail = filesystem_error("volume_archive.publish", ec.value());
        return false;
    }
    published_result = true;
    std::filesystem::remove(temporary, ec);
    if (ec) {
        failure_detail = filesystem_error("volume_archive.remove_temp", ec.value());
        return false;
    }
    return true;
}

bool valid_request(const FilesVolumeArchiveWriteRequest &request) {
    return request.destination_directory.is_absolute() && safe_archive_name(request.archive_name) &&
           request.transaction_id != 0;
}

std::optional<ck::files::NativeName> archive_journal_name(const ck::files::NativeAbsolutePath &root,
                                                          const ck::files::OperationJournalId &id) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text = ".ck-volume-archive-";
    for (const auto byte : id.bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0FU]);
    }
    if (root.encoding() == ck::files::NameEncoding::PosixBytes)
        return ck::files::NativeName::from_posix(std::move(text));
    if (root.encoding() == ck::files::NameEncoding::WindowsUtf16) {
        std::u16string units;
        units.reserve(text.size());
        for (const auto character : text)
            units.push_back(static_cast<char16_t>(static_cast<unsigned char>(character)));
        return ck::files::NativeName::from_windows(std::move(units));
    }
    return std::nullopt;
}

std::optional<ck::files::NativeName> archive_staging_name(const ck::files::NativeAbsolutePath &destination,
                                                          std::string_view archive_name,
                                                          std::uint64_t transaction_id) {
    const auto text = "." + std::string(archive_name) + ".cktmp-" + std::to_string(transaction_id);
    if (destination.encoding() == ck::files::NameEncoding::PosixBytes)
        return ck::files::NativeName::from_posix(text);
    if (destination.encoding() == ck::files::NameEncoding::WindowsUtf16) {
        std::u16string units;
        units.reserve(text.size());
        for (const auto character : text)
            units.push_back(static_cast<char16_t>(static_cast<unsigned char>(character)));
        return ck::files::NativeName::from_windows(std::move(units));
    }
    return std::nullopt;
}

struct ArchiveJournalBinding final {
    ck::files::OperationJournal::Pointer journal;
    std::shared_ptr<ck::files::OperationJournalFile> file;
    std::vector<std::byte> evidence;
    std::optional<std::size_t> staging_flags_offset;

    ck::files::Observation<bool> append(ck::files::OperationJournalState state) {
        const auto sequence = static_cast<std::uint64_t>(journal->records().size()) + 1;
        return ck::files::append_operation_journal_file(
            *file, *journal, {sequence, 0, state, evidence});
    }

    ck::files::Observation<bool> append_staging_digest(
        ck::files::OperationJournalState state, std::span<const std::byte> digest) {
        if (digest.size() != 32 || !staging_flags_offset || *staging_flags_offset >= evidence.size() ||
            std::to_integer<std::uint8_t>(evidence[*staging_flags_offset]) != kStagingPathEvidence)
            return ck::files::Observation<bool>::unavailable(
                ObservationState::Failed, error(AdapterError::PlanInvalid, "volume_archive.staging_evidence"));
        evidence[*staging_flags_offset] = std::byte{kStagingPathEvidence | kStagingDigestEvidence};
        evidence.insert(evidence.end(), digest.begin(), digest.end());
        return append(state);
    }
};

} // namespace

Observation<FilesVolumeArchiveRecoveryReport> inspect_volume_archive_recovery(
    const ck::files::OperationJournal &journal, std::optional<UnavailableObservation> tail) {
    if (journal.operation() != ck::files::FileOperation::Copy)
        return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
            ObservationState::Unsupported, error(AdapterError::PlanInvalid, "volume_archive.recovery_operation"));
    auto operation = ck::files::inspect_operation_recovery(journal, std::move(tail));
    if (!operation.value())
        return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
            operation.state(), operation.unavailable_detail()->error);

    std::optional<FilesVolumeArchiveJournalEvidence> evidence;
    for (const auto &record : journal.records()) {
        if (record.item != 0)
            return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
                ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_item"));
        auto decoded = decode_archive_journal_evidence(record.evidence);
        if (!decoded.value())
            return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
                decoded.state(), decoded.unavailable_detail()->error);
        if (evidence) {
            const auto &current = *decoded.value();
            const bool identity_changed = evidence->transaction_id != current.transaction_id ||
                evidence->source_identity != current.source_identity ||
                evidence->source_mount_point != current.source_mount_point ||
                evidence->archive_name != current.archive_name ||
                evidence->destination_encoding != current.destination_encoding ||
                evidence->destination_native_path != current.destination_native_path ||
                evidence->staging_encoding != current.staging_encoding ||
                evidence->staging_native_path != current.staging_native_path ||
                (evidence->staging_digest && !current.staging_digest) ||
                (evidence->staging_digest && current.staging_digest &&
                 *evidence->staging_digest != *current.staging_digest);
            if (identity_changed)
                return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
                    ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.recovery_identity"));
        }
        evidence = std::move(decoded).take_value();
    }
    if (!evidence)
        return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
            ObservationState::Unknown, error(AdapterError::PlanInvalid, "volume_archive.recovery_evidence"));
    return Observation<FilesVolumeArchiveRecoveryReport>::available(
        {journal.id(), std::move(*evidence), std::move(operation).take_value()});
}

Observation<FilesVolumeArchiveRecoveryReport> inspect_volume_archive_recovery(
    ck::files::OperationJournalFile &file, ck::files::OperationJournalLimits journal_limits,
    ck::files::OperationJournalCodecLimits codec_limits) {
    auto loaded = ck::files::load_operation_journal(file, journal_limits, codec_limits);
    if (!loaded.value())
        return Observation<FilesVolumeArchiveRecoveryReport>::unavailable(
            loaded.state(), loaded.unavailable_detail()->error);
    return inspect_volume_archive_recovery(*loaded.value()->journal, loaded.value()->tail);
}

Observation<FilesVolumeArchiveRecoveryScan> discover_volume_archive_recovery(
    const ck::files::NativeAbsolutePath &private_root, FilesVolumeArchiveRecoveryLimits limits) {
    if (!private_root.is_valid() || limits.max_journals == 0 || limits.max_journals > 16 * 1024 ||
        !limits.journal.max_items || !limits.journal.max_records || !limits.codec.max_bytes)
        return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
            ObservationState::Failed, error(AdapterError::InvalidLimit, "volume_archive.recovery_scan_limits"));
    auto host = private_root.host_path();
    if (!host.value())
        return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
            host.state(), host.unavailable_detail()->error);
    std::error_code ec;
    const auto root_status = std::filesystem::symlink_status(*host.value(), ec);
    if (ec)
        return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
            ObservationState::Denied, filesystem_error("volume_archive.recovery_root_status", ec.value()).error);
    if (!std::filesystem::is_directory(root_status) || std::filesystem::is_symlink(root_status))
        return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
            ObservationState::Unsupported, error(AdapterError::NotDirectory, "volume_archive.recovery_root"));

    FilesVolumeArchiveRecoveryScan result;
    std::filesystem::directory_iterator iterator(*host.value(), ec);
    if (ec)
        return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
            ObservationState::Denied, filesystem_error("volume_archive.recovery_enumerate", ec.value()).error);
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(ec)) {
        if (ec)
            return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
                ObservationState::Denied, filesystem_error("volume_archive.recovery_enumerate", ec.value()).error);
        const auto entry = *iterator;
        const auto basename = entry.path().filename().string();
        if (!is_archive_journal_name(basename))
            continue;
        if (result.journals.size() >= limits.max_journals)
            return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
                ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.recovery_count"));
        FilesVolumeArchiveRecoveryItem item;
        item.display_name = basename;
        item.native_name = native_journal_name(entry.path());
        const auto status = entry.symlink_status(ec);
        if (ec) {
            item.error = filesystem_error("volume_archive.recovery_entry_status", ec.value());
            result.requires_reconciliation = true;
            result.journals.push_back(std::move(item));
            ec.clear();
            continue;
        }
        if (!item.native_name || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
            item.error = error(AdapterError::UnsupportedQuery, "volume_archive.recovery_entry_kind");
            result.requires_reconciliation = true;
            result.journals.push_back(std::move(item));
            continue;
        }
        auto file = ck::files::open_native_operation_journal_file(private_root, *item.native_name, false);
        if (!file.value()) {
            item.error = *file.unavailable_detail();
            result.requires_reconciliation = true;
        } else {
            auto report = inspect_volume_archive_recovery(**file.value(), limits.journal, limits.codec);
            if (report.value()) {
                result.requires_reconciliation = result.requires_reconciliation ||
                    report.value()->operation.requires_reconciliation;
                item.report = std::move(report).take_value();
            } else {
                item.error = *report.unavailable_detail();
                result.requires_reconciliation = true;
            }
        }
        result.journals.push_back(std::move(item));
    }
    if (ec)
        return Observation<FilesVolumeArchiveRecoveryScan>::unavailable(
            ObservationState::Denied, filesystem_error("volume_archive.recovery_enumerate", ec.value()).error);
    return Observation<FilesVolumeArchiveRecoveryScan>::available(std::move(result));
}

struct FilesVolumeArchiveRecoveryService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<ck::files::NativeAbsolutePath, FilesVolumeArchiveRecoveryLimits>> pending;
    std::optional<FilesVolumeArchiveRecoveryReply> reply;
    std::stop_source stop;
    bool stopping = false, busy = false, signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        stop.request_stop();
        condition.notify_all();
    }
    void join() {
        if (worker.joinable())
            worker.join();
    }
    void signal() {
        if (signalled || !wake)
            return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    static bool valid(const ck::files::NativeAbsolutePath &root,
                      const FilesVolumeArchiveRecoveryLimits &limits) {
        return root.is_valid() && limits.max_journals != 0 && limits.max_journals <= 16 * 1024 &&
               limits.journal.max_items != 0 && limits.journal.max_records != 0 && limits.codec.max_bytes != 0;
    }
    void emit(FilesVolumeArchiveRecoveryReply value) {
        std::lock_guard lock(mutex);
        if (stopping)
            return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    void run_one(ck::files::NativeAbsolutePath root, FilesVolumeArchiveRecoveryLimits limits) {
        try {
            auto scan = discover_volume_archive_recovery(root, limits);
            FilesVolumeArchiveRecoveryReply result;
            if (scan.value()) {
                result.state = FilesVolumeArchiveRecoveryState::Ready;
                result.scan = std::move(scan).take_value();
            } else {
                result.state = FilesVolumeArchiveRecoveryState::Failed;
                result.error = *scan.unavailable_detail();
            }
            emit(std::move(result));
        } catch (...) {
            FilesVolumeArchiveRecoveryReply result;
            result.state = FilesVolumeArchiveRecoveryState::WorkerFault;
            result.error = error(AdapterError::PlanInvalid, "volume_archive.recovery.worker_fault");
            emit(std::move(result));
        }
    }
    void run() {
        for (;;) {
            std::optional<std::pair<ck::files::NativeAbsolutePath, FilesVolumeArchiveRecoveryLimits>> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) {
                    pending.reset();
                    return;
                }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                stop = std::stop_source{};
                signalled = false;
            }
            run_one(std::move(job->first), std::move(job->second));
        }
    }
};

FilesVolumeArchiveRecoveryService::FilesVolumeArchiveRecoveryService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesVolumeArchiveRecoveryService::~FilesVolumeArchiveRecoveryService() = default;
FilesVolumeArchiveRecoverySubmission FilesVolumeArchiveRecoveryService::request(
    ck::files::NativeAbsolutePath private_root, FilesVolumeArchiveRecoveryLimits limits) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return FilesVolumeArchiveRecoverySubmission::Stopped;
    if (!State::valid(private_root, limits))
        return FilesVolumeArchiveRecoverySubmission::Invalid;
    if (state_->busy || state_->pending)
        return FilesVolumeArchiveRecoverySubmission::Busy;
    state_->pending = std::make_pair(std::move(private_root), limits);
    state_->condition.notify_one();
    return FilesVolumeArchiveRecoverySubmission::Accepted;
}
std::optional<FilesVolumeArchiveRecoveryReply> FilesVolumeArchiveRecoveryService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesVolumeArchiveRecoveryService::request_stop() noexcept { state_->request_stop(); }
void FilesVolumeArchiveRecoveryService::join() { state_->join(); }
bool FilesVolumeArchiveRecoveryService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesVolumeArchiveRecoveryService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

namespace {

Observation<bool> forward_cleanup_failure(const UnavailableObservation &detail) {
    return Observation<bool>::unavailable(detail.state, detail.error);
}

Observation<std::array<std::byte, 32>> digest_opened_archive(
    const ck::files::OpenedEntry &entry, std::uint64_t max_bytes) {
    constexpr std::size_t chunk_size = 64 * 1024;
    ck::files::Sha256 digest;
    std::uint64_t offset = 0;
    for (;;) {
        auto content = entry.read_content({chunk_size}, offset);
        if (!content.value())
            return Observation<std::array<std::byte, 32>>::unavailable(
                content.state(), content.unavailable_detail()->error);
        if (content.value()->bytes.size() > max_bytes - std::min(offset, max_bytes))
            return Observation<std::array<std::byte, 32>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.cleanup_bytes"));
        if (content.value()->bytes.empty() && !content.value()->complete)
            return Observation<std::array<std::byte, 32>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InvalidNativeResult,
                                                  "volume_archive.cleanup_read_progress"));
        digest.update(std::span<const std::byte>(content.value()->bytes));
        offset += content.value()->bytes.size();
        if (content.value()->complete)
            return Observation<std::array<std::byte, 32>>::available(digest.digest());
        if (offset >= max_bytes)
            return Observation<std::array<std::byte, 32>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.cleanup_bytes"));
    }
}

Observation<bool> cleanup_volume_archive_recovery(
    const ck::files::NativeAbsolutePath &private_root, const ck::files::NativeName &journal_name,
    const ck::files::OperationJournalId &journal_id, FilesVolumeArchiveRecoveryLimits journal_limits,
    FilesVolumeArchiveRecoveryCleanupLimits cleanup_limits) {
    if (!private_root.is_valid() || !journal_name.is_valid() ||
        !cleanup_limits.max_staging_bytes ||
        cleanup_limits.max_staging_bytes > FilesVolumeArchiveRecoveryCleanupLimits::hard_max_staging_bytes)
        return Observation<bool>::unavailable(
            ObservationState::Failed, error(AdapterError::InvalidLimit, "volume_archive.cleanup_limits"));
    auto expected_journal_name = archive_journal_name(private_root, journal_id);
    if (!expected_journal_name || *expected_journal_name != journal_name)
        return Observation<bool>::unavailable(
            ObservationState::Changed, error(AdapterError::PlanDecisionStale, "volume_archive.cleanup_journal"));
    auto file = ck::files::open_native_operation_journal_file(private_root, journal_name, false);
    if (!file.value())
        return forward_cleanup_failure(*file.unavailable_detail());
    auto loaded = ck::files::load_operation_journal(**file.value(), journal_limits.journal, journal_limits.codec);
    if (!loaded.value())
        return forward_cleanup_failure(*loaded.unavailable_detail());
    if (loaded.value()->journal->id() != journal_id || loaded.value()->tail)
        return Observation<bool>::unavailable(
            ObservationState::Unknown, error(AdapterError::PlanDecisionStale, "volume_archive.cleanup_journal"));
    auto report = inspect_volume_archive_recovery(*loaded.value()->journal, loaded.value()->tail);
    if (!report.value())
        return forward_cleanup_failure(*report.unavailable_detail());
    if (report.value()->operation.items.size() != 1 ||
        report.value()->operation.items.front().item != 0 ||
        report.value()->operation.items.front().action != ck::files::OperationRecoveryAction::InspectUnpublishedStaging ||
        (report.value()->operation.items.front().state != ck::files::OperationJournalState::Copied &&
         report.value()->operation.items.front().state != ck::files::OperationJournalState::Verified) ||
        !report.value()->evidence.destination_encoding || report.value()->evidence.destination_native_path.empty() ||
        !report.value()->evidence.staging_encoding || report.value()->evidence.staging_native_path.empty() ||
        !report.value()->evidence.staging_digest)
        return Observation<bool>::unavailable(
            ObservationState::Unknown, error(AdapterError::PlanDecisionStale, "volume_archive.cleanup_state"));

    auto destination = decode_native_absolute_path(
        *report.value()->evidence.destination_encoding, report.value()->evidence.destination_native_path);
    if (!destination.value())
        return forward_cleanup_failure(*destination.unavailable_detail());
    auto staging = decode_native_absolute_path(
        *report.value()->evidence.staging_encoding, report.value()->evidence.staging_native_path);
    if (!staging.value())
        return forward_cleanup_failure(*staging.unavailable_detail());
    auto staging_name = archive_staging_name(
        *destination.value(), report.value()->evidence.archive_name, report.value()->evidence.transaction_id);
    if (!staging_name)
        return Observation<bool>::unavailable(
            ObservationState::Unsupported, error(AdapterError::UnrepresentableName, "volume_archive.cleanup_name"));
    auto expected_staging = destination.value()->child_address(*staging_name);
    if (!expected_staging.value() || *expected_staging.value() != *staging.value())
        return Observation<bool>::unavailable(
            ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.cleanup_staging"));

    ck::files::NativeFileSystem filesystem;
    auto destination_cursor = filesystem.open_native_directory(*destination.value());
    if (!destination_cursor.value())
        return forward_cleanup_failure(*destination_cursor.unavailable_detail());
    auto child = (*destination_cursor.value())->open_child(*staging_name, ck::files::EntryAccess::ContentRead);
    if (!child.value())
        return forward_cleanup_failure(*child.unavailable_detail());
    auto metadata = (*child.value())->inspect_basic();
    if (!metadata.value() || !metadata.value()->kind.value() ||
        *metadata.value()->kind.value() != ck::files::ObjectKind::RegularFile ||
        !metadata.value()->identity.value())
        return Observation<bool>::unavailable(
            ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.cleanup_kind"));
    auto digest = digest_opened_archive(**child.value(), cleanup_limits.max_staging_bytes);
    if (!digest.value())
        return forward_cleanup_failure(*digest.unavailable_detail());
    if (*digest.value() != *report.value()->evidence.staging_digest)
        return Observation<bool>::unavailable(
            ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.cleanup_digest"));
    auto discarded = (*destination_cursor.value())->discard_child(
        *staging_name, *metadata.value()->identity.value());
    if (!discarded.value())
        return forward_cleanup_failure(*discarded.unavailable_detail());
    if (!*discarded.value())
        return Observation<bool>::unavailable(
            ObservationState::Unknown, error(AdapterError::InvalidNativeResult, "volume_archive.cleanup_result"));

    const auto sequence = static_cast<std::uint64_t>(loaded.value()->journal->records().size()) + 1;
    auto cancelled = ck::files::append_operation_journal_file(
        **file.value(), *loaded.value()->journal,
        {sequence, 0, ck::files::OperationJournalState::Cancelled,
         loaded.value()->journal->records().back().evidence});
    if (!cancelled.value())
        return forward_cleanup_failure(*cancelled.unavailable_detail());
    return Observation<bool>::available(true);
}

} // namespace

struct FilesVolumeArchiveRecoveryCleanupService::State final {
    using Job = std::tuple<ck::files::NativeAbsolutePath, ck::files::NativeName,
                           ck::files::OperationJournalId, FilesVolumeArchiveRecoveryLimits,
                           FilesVolumeArchiveRecoveryCleanupLimits>;
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Job> pending;
    std::optional<FilesVolumeArchiveRecoveryCleanupReply> reply;
    std::stop_source stop;
    bool stopping = false, busy = false, signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        stop.request_stop();
        condition.notify_all();
    }
    void join() {
        if (worker.joinable()) worker.join();
    }
    void signal() {
        if (signalled || !wake) return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    void emit(FilesVolumeArchiveRecoveryCleanupReply value) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    void run_one(Job job) {
        try {
            auto result = cleanup_volume_archive_recovery(
                std::get<0>(job), std::get<1>(job), std::get<2>(job), std::get<3>(job), std::get<4>(job));
            FilesVolumeArchiveRecoveryCleanupReply local_reply;
            if (result.value()) {
                local_reply.state = FilesVolumeArchiveRecoveryCleanupState::Complete;
                local_reply.removed = true;
            } else {
                local_reply.state = FilesVolumeArchiveRecoveryCleanupState::Failed;
                local_reply.error = *result.unavailable_detail();
            }
            emit(std::move(local_reply));
        } catch (...) {
            FilesVolumeArchiveRecoveryCleanupReply local_reply;
            local_reply.state = FilesVolumeArchiveRecoveryCleanupState::WorkerFault;
            local_reply.error = error(AdapterError::PlanInvalid, "volume_archive.cleanup.worker_fault");
            emit(std::move(local_reply));
        }
    }
    void run() {
        for (;;) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) { pending.reset(); return; }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                stop = std::stop_source{};
                signalled = false;
            }
            run_one(std::move(*job));
        }
    }
};

FilesVolumeArchiveRecoveryCleanupService::FilesVolumeArchiveRecoveryCleanupService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesVolumeArchiveRecoveryCleanupService::~FilesVolumeArchiveRecoveryCleanupService() = default;
FilesVolumeArchiveRecoveryCleanupSubmission FilesVolumeArchiveRecoveryCleanupService::request(
    ck::files::NativeAbsolutePath private_root, ck::files::NativeName journal_name,
    ck::files::OperationJournalId journal_id, FilesVolumeArchiveRecoveryLimits journal_limits,
    FilesVolumeArchiveRecoveryCleanupLimits cleanup_limits) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return FilesVolumeArchiveRecoveryCleanupSubmission::Stopped;
    if (!private_root.is_valid() || !journal_name.is_valid() ||
        !cleanup_limits.max_staging_bytes ||
        cleanup_limits.max_staging_bytes > FilesVolumeArchiveRecoveryCleanupLimits::hard_max_staging_bytes)
        return FilesVolumeArchiveRecoveryCleanupSubmission::Invalid;
    if (state_->busy || state_->pending) return FilesVolumeArchiveRecoveryCleanupSubmission::Busy;
    state_->pending.emplace(std::move(private_root), std::move(journal_name), journal_id,
                            journal_limits, cleanup_limits);
    state_->condition.notify_one();
    return FilesVolumeArchiveRecoveryCleanupSubmission::Accepted;
}
std::optional<FilesVolumeArchiveRecoveryCleanupReply> FilesVolumeArchiveRecoveryCleanupService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesVolumeArchiveRecoveryCleanupService::request_stop() noexcept { state_->request_stop(); }
void FilesVolumeArchiveRecoveryCleanupService::join() { state_->join(); }
bool FilesVolumeArchiveRecoveryCleanupService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesVolumeArchiveRecoveryCleanupService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

Observation<FilesVolumeArchiveWriteResult> write_volume_archive(const FilesVolumeArchiveWriteRequest &request,
                                                                std::stop_token stop_token) {
    if (!valid_request(request))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unsupported, error(AdapterError::PlanInvalid, "volume_archive.request"));
    if (!request.document.losses.empty() && !request.document.losses_reviewed)
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unknown, error(AdapterError::ProviderReviewRequired, "volume_archive.loss_consent"));
    if (std::any_of(request.document.losses.begin(), request.document.losses.end(),
                    [](const auto &loss) { return loss.hard_failure; }))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unsupported, error(AdapterError::ProviderReviewRequired, "volume_archive.hard_loss"));
    std::error_code ec;
    const auto destination_status = std::filesystem::symlink_status(request.destination_directory, ec);
    if (ec)
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Denied, filesystem_error("volume_archive.destination_status", ec.value()).error);
    if (!std::filesystem::is_directory(destination_status) || std::filesystem::is_symlink(destination_status))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unsupported, error(AdapterError::NotDirectory, "volume_archive.destination"));

    std::uint64_t data_size = 0, data_bytes = 0;
    auto encoded_observation = validate_and_encode(request.document, data_size, data_bytes, stop_token);
    if (!encoded_observation.value())
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            encoded_observation.unavailable_detail()->state, encoded_observation.unavailable_detail()->error);
    auto encoded = std::move(encoded_observation).take_value();
    if (stop_token.stop_requested())
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unknown, error(AdapterError::InterruptedRead, "volume_archive.cancelled"));

    std::vector<std::byte> manifest;
    append_manifest(manifest, request.document, encoded);
    std::uint64_t data_offset = 0;
    if (!checked_add(kHeaderSize, manifest.size(), data_offset))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.manifest_size"));
    std::uint64_t logical_size = 0;
    if (!checked_add(data_offset, data_size, logical_size))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unknown, error(AdapterError::InventoryLimit, "volume_archive.file_size"));

    const auto published = request.destination_directory / request.archive_name;
    const auto temp = request.destination_directory /
                      ("." + request.archive_name + ".cktmp-" + std::to_string(request.transaction_id));
    UnavailableObservation failure_detail;
    if (std::filesystem::exists(published, ec) || ec)
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Changed, error(AdapterError::PlanDecisionStale, "volume_archive.destination_conflict"));
    if (!create_exclusive(temp, failure_detail))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(failure_detail.state, failure_detail.error);
    if (!enable_sparse(temp, failure_detail)) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(failure_detail.state, failure_detail.error);
    }
    std::fstream stream(temp, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Failed, error(AdapterError::InvalidPath, "volume_archive.open_temp"));
    }

    std::vector<std::byte> header;
    append_header(header, request.document.consistency == FilesVolumeArchiveConsistency::Snapshot ? 1U : 0U,
                  manifest.size(), data_offset, data_size, encoded.size());
    stream.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
    stream.write(reinterpret_cast<const char *>(manifest.data()), static_cast<std::streamsize>(manifest.size()));
    if (!stream) {
        stream.close();
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Failed, error(AdapterError::InvalidPath, "volume_archive.write_manifest"));
    }
    for (const auto &entry : encoded) {
        if (stop_token.stop_requested()) {
            stream.close();
            std::filesystem::remove(temp, ec);
            return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                ObservationState::Unknown, error(AdapterError::InterruptedRead, "volume_archive.cancelled"));
        }
        for (const auto &extent : entry.extents) {
            stream.seekp(static_cast<std::streamoff>(data_offset + extent.archive_offset));
            stream.write(reinterpret_cast<const char *>(extent.bytes->data()),
                         static_cast<std::streamsize>(extent.bytes->size()));
            if (!stream) {
                stream.close();
                std::filesystem::remove(temp, ec);
                return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                    ObservationState::Failed, error(AdapterError::InvalidPath, "volume_archive.write_extent"));
            }
        }
    }
    stream.flush();
    stream.close();
    if (!stream) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Failed, error(AdapterError::InvalidPath, "volume_archive.flush_stream"));
    }
    std::filesystem::resize_file(temp, logical_size, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Failed, filesystem_error("volume_archive.resize", ec.value()).error);
    }
    if (!durable_flush(temp, failure_detail)) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(failure_detail.state, failure_detail.error);
    }
    auto staging_digest = digest_file(temp);
    if (!staging_digest.value()) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            staging_digest.state(), staging_digest.unavailable_detail()->error);
    }
    if (request.append_journal_evidence) {
        auto journal = request.append_journal_evidence(
            ck::files::OperationJournalState::Copied,
            std::span<const std::byte>(staging_digest.value()->data(), staging_digest.value()->size()));
        if (!journal.value()) {
            std::filesystem::remove(temp, ec);
            return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                journal.unavailable_detail()->state, journal.unavailable_detail()->error);
        }
    } else if (request.append_journal) {
        auto journal = request.append_journal(ck::files::OperationJournalState::Copied);
        if (!journal.value()) {
            std::filesystem::remove(temp, ec);
            return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                journal.unavailable_detail()->state, journal.unavailable_detail()->error);
        }
    }

    std::ifstream verify(temp, std::ios::binary);
    std::vector<char> header_check(header.size());
    verify.read(header_check.data(), static_cast<std::streamsize>(header_check.size()));
    if (!verify || std::memcmp(header_check.data(), header.data(), header.size()) != 0) {
        verify.close();
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.verify_header"));
    }
    std::vector<char> bytes_check;
    for (const auto &entry : encoded)
        for (const auto &extent : entry.extents) {
            verify.seekg(static_cast<std::streamoff>(data_offset + extent.archive_offset));
            bytes_check.resize(extent.bytes->size());
            verify.read(bytes_check.data(), static_cast<std::streamsize>(bytes_check.size()));
            ck::files::Sha256 expected_digest;
            expected_digest.update(std::span<const std::byte>(*extent.bytes));
            ck::files::Sha256 actual_digest;
            actual_digest.update(std::as_bytes(std::span<const char>(bytes_check)));
            if (!verify || expected_digest.digest() != actual_digest.digest()) {
                verify.close();
                std::filesystem::remove(temp, ec);
                return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                    ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.verify_extent"));
            }
        }
    verify.close();
    if (request.append_journal) {
        auto journal = request.append_journal(ck::files::OperationJournalState::Verified);
        if (!journal.value()) {
            std::filesystem::remove(temp, ec);
            return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                journal.unavailable_detail()->state, journal.unavailable_detail()->error);
        }
    }
    if (request.append_journal) {
        auto journal = request.append_journal(ck::files::OperationJournalState::CommitIntended);
        if (!journal.value()) {
            std::filesystem::remove(temp, ec);
            return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                journal.unavailable_detail()->state, journal.unavailable_detail()->error);
        }
    }
    bool published_result = false;
    if (!publish_without_replace(temp, published, failure_detail, published_result)) {
        if (published_result) {
            if (request.append_journal)
                (void)request.append_journal(ck::files::OperationJournalState::DestinationCommitted);
            const auto diagnostic = failure_detail.error.operation;
            if (!durable_directory(request.destination_directory, failure_detail)) {
                if (request.append_journal)
                    (void)request.append_journal(ck::files::OperationJournalState::NeedsReconciliation);
                return Observation<FilesVolumeArchiveWriteResult>::available(
                    {published, logical_size, data_bytes, encoded.size(),
                     FilesVolumeArchivePublication::PublishedDurabilityUnknown,
                     failure_detail.error.operation, true});
            }
            return Observation<FilesVolumeArchiveWriteResult>::available(
                {published, logical_size, data_bytes, encoded.size(),
                 FilesVolumeArchivePublication::PublishedDurabilityUnknown, diagnostic, true});
        }
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(failure_detail.state, failure_detail.error);
    }
    if (request.append_journal) {
        auto journal = request.append_journal(ck::files::OperationJournalState::DestinationCommitted);
        if (!journal.value())
            return Observation<FilesVolumeArchiveWriteResult>::available(
                {published, logical_size, data_bytes, encoded.size(),
                 FilesVolumeArchivePublication::PublishedDurabilityUnknown,
                 journal.unavailable_detail()->error.operation, true});
    }
    if (!durable_directory(request.destination_directory, failure_detail)) {
        if (request.append_journal)
            (void)request.append_journal(ck::files::OperationJournalState::NeedsReconciliation);
        return Observation<FilesVolumeArchiveWriteResult>::available(
            {published, logical_size, data_bytes, encoded.size(),
             FilesVolumeArchivePublication::PublishedDurabilityUnknown, failure_detail.error.operation, true});
    }
    if (request.append_journal) {
        auto journal = request.append_journal(ck::files::OperationJournalState::Complete);
        if (!journal.value())
            return Observation<FilesVolumeArchiveWriteResult>::available(
                {published, logical_size, data_bytes, encoded.size(),
                 FilesVolumeArchivePublication::PublishedDurabilityUnknown,
                 journal.unavailable_detail()->error.operation, true});
    }
    return Observation<FilesVolumeArchiveWriteResult>::available(
        {published, logical_size, data_bytes, encoded.size(), FilesVolumeArchivePublication::Durable, {}, true});
}

namespace {

template <class T>
Observation<T> review_rejected(ObservationState state, AdapterError code, const char *operation) {
    return Observation<T>::unavailable(state, error(code, operation));
}

bool all_loss_indices_reviewed(std::span<const std::size_t> reviewed, std::size_t count) {
    if (reviewed.size() != count)
        return false;
    std::vector<bool> seen(count, false);
    for (const auto index : reviewed) {
        if (index >= count || seen[index])
            return false;
        seen[index] = true;
    }
    return true;
}

} // namespace

std::string FilesVolumeArchiveReview::default_archive_name(std::string_view volume_name) {
    std::string stem;
    stem.reserve(std::min<std::size_t>(volume_name.size(), 128));
    for (const unsigned char byte : volume_name) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.')
            stem.push_back(static_cast<char>(byte));
        else
            stem.push_back('_');
        if (stem.size() == 128)
            break;
    }
    if (stem.empty() || stem == "." || stem == "..")
        stem = "volume";
    if (!stem.ends_with(".ckarchive"))
        stem += ".ckarchive";
    return stem;
}

Observation<FilesVolumeArchiveReview::Pointer> FilesVolumeArchiveReview::create(
    FilesVolumeArchiveDocument document, ck::files::NativeAbsolutePath destination_directory,
    std::string archive_name, FilesVolumeArchiveDestinationState destination_state) {
    if (!document.source.identity.empty() && !document.source.mount_point.empty()) {
        // Continue below; the explicit checks are kept separate so every
        // refusal has a stable operation name for the status/review layer.
    } else {
        return review_rejected<Pointer>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity,
                                        "volume_archive.review.source_identity");
    }
    if (!destination_directory.is_valid())
        return review_rejected<Pointer>(ObservationState::Failed, AdapterError::InvalidPath,
                                        "volume_archive.review.destination");
    if (archive_name.empty())
        archive_name = default_archive_name(document.source.name);
    if (!safe_archive_name(archive_name))
        return review_rejected<Pointer>(ObservationState::Unsupported, AdapterError::UnrepresentableName,
                                        "volume_archive.review.archive_name");
    if (destination_state == FilesVolumeArchiveDestinationState::Unknown)
        return review_rejected<Pointer>(ObservationState::Unknown, AdapterError::PlanDecisionStale,
                                        "volume_archive.review.destination_state");

    std::uint64_t logical_bytes = 0;
    std::uint64_t data_bytes = 0;
    auto encoded = validate_and_encode(document, logical_bytes, data_bytes, {});
    if (!encoded.value())
        return review_rejected<Pointer>(encoded.unavailable_detail()->state,
                                        static_cast<AdapterError>(encoded.unavailable_detail()->error.code),
                                        "volume_archive.review.document");

    FilesVolumeArchiveReviewSummary summary;
    summary.logical_bytes = logical_bytes;
    summary.data_bytes = data_bytes;
    summary.entries = document.entries.size();
    summary.destination_conflict = destination_state == FilesVolumeArchiveDestinationState::Conflict;
    for (const auto &entry : document.entries) {
        switch (entry.kind) {
        case FilesVolumeArchiveEntryKind::Directory: ++summary.directories; break;
        case FilesVolumeArchiveEntryKind::RegularFile:
            ++summary.regular_files;
            if (!entry.extents.empty()) {
                const auto &last = entry.extents.back();
                std::uint64_t end = last.offset + last.bytes.size();
                if (end < entry.logical_size)
                    ++summary.sparse_files;
            }
            break;
        case FilesVolumeArchiveEntryKind::Symlink: ++summary.links; break;
        case FilesVolumeArchiveEntryKind::HardLink: ++summary.hard_links; break;
        case FilesVolumeArchiveEntryKind::Unsupported: ++summary.unsupported_objects; break;
        }
    }
    summary.losses = document.losses.size();
    summary.hard_losses = static_cast<std::size_t>(std::count_if(
        document.losses.begin(), document.losses.end(), [](const auto &loss) { return loss.hard_failure; }));
    return Observation<Pointer>::available(Pointer(new FilesVolumeArchiveReview(
        std::move(document), std::move(destination_directory), std::move(archive_name), summary)));
}

namespace {

Observation<FilesVolumeArchiveDestinationState> probe_archive_destination(
    const ck::files::NativeAbsolutePath &destination_directory, std::string_view archive_name) {
    auto host = destination_directory.host_path();
    if (!host.value())
        return Observation<FilesVolumeArchiveDestinationState>::unavailable(
            host.unavailable_detail()->state, host.unavailable_detail()->error);
    std::error_code ec;
    const auto directory_status = std::filesystem::symlink_status(*host.value(), ec);
    if (ec)
        return Observation<FilesVolumeArchiveDestinationState>::unavailable(
            ObservationState::Denied, filesystem_error("volume_archive.plan.destination", ec.value()).error);
    if (!std::filesystem::exists(directory_status))
        return Observation<FilesVolumeArchiveDestinationState>::unavailable(
            ObservationState::Unknown, error(AdapterError::PlanDecisionStale, "volume_archive.plan.destination").error);
    if (!std::filesystem::is_directory(directory_status))
        return Observation<FilesVolumeArchiveDestinationState>::unavailable(
            ObservationState::Unsupported, error(AdapterError::NotDirectory, "volume_archive.plan.destination").error);

    const auto target = *host.value() / std::filesystem::path(std::string(archive_name));
    const auto target_status = std::filesystem::symlink_status(target, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return Observation<FilesVolumeArchiveDestinationState>::unavailable(
            ObservationState::Unknown, filesystem_error("volume_archive.plan.conflict", ec.value()).error);
    if (ec == std::errc::no_such_file_or_directory)
        return Observation<FilesVolumeArchiveDestinationState>::available(FilesVolumeArchiveDestinationState::Ready);
    return Observation<FilesVolumeArchiveDestinationState>::available(
        std::filesystem::exists(target_status) ? FilesVolumeArchiveDestinationState::Conflict
                                                : FilesVolumeArchiveDestinationState::Ready);
}

} // namespace

Observation<FilesVolumeArchiveApproval> FilesVolumeArchiveReview::approve(
    FilesVolumeArchiveReviewDecision decision, std::span<const std::size_t> reviewed_losses) const {
    if (decision == FilesVolumeArchiveReviewDecision::Cancel)
        return review_rejected<FilesVolumeArchiveApproval>(ObservationState::Unsupported,
                                                           AdapterError::ProviderReviewRequired,
                                                           "volume_archive.review.cancelled");
    if (summary_.destination_conflict)
        return review_rejected<FilesVolumeArchiveApproval>(ObservationState::Changed,
                                                           AdapterError::PlanDecisionStale,
                                                           "volume_archive.review.destination_conflict");
    if (document_.losses.empty()) {
        if (decision != FilesVolumeArchiveReviewDecision::Proceed || !reviewed_losses.empty())
            return review_rejected<FilesVolumeArchiveApproval>(ObservationState::Unsupported,
                                                               AdapterError::ProviderReviewRequired,
                                                               "volume_archive.review.loss_decision");
        return Observation<FilesVolumeArchiveApproval>::available({shared_from_this(), false});
    }
    if (decision != FilesVolumeArchiveReviewDecision::ContinueWithListedLosses ||
        !all_loss_indices_reviewed(reviewed_losses, document_.losses.size()) || summary_.hard_losses != 0)
        return review_rejected<FilesVolumeArchiveApproval>(ObservationState::Unsupported,
                                                           AdapterError::ProviderReviewRequired,
                                                           "volume_archive.review.loss_consent");
    return Observation<FilesVolumeArchiveApproval>::available({shared_from_this(), true});
}

struct FilesVolumeArchivePlanningService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<FilesVolumeArchivePlanningTicket, FilesVolumeArchivePlanningRequest>> pending;
    std::optional<FilesVolumeArchivePlanningReply> reply;
    FilesVolumeArchivePlanningTicket latest;
    std::uint64_t next = 1;
    std::stop_source stop;
    bool stopping = false, busy = false, signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        stop.request_stop();
        condition.notify_all();
    }
    void join() {
        if (worker.joinable())
            worker.join();
    }
    void signal() {
        if (signalled || !wake)
            return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    void emit(FilesVolumeArchivePlanningReply value) {
        std::lock_guard lock(mutex);
        if (stopping || value.request != latest)
            return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    static bool valid(const FilesVolumeArchivePlanningRequest &value) {
        return !value.source.identity.empty() && !value.source.mount_point.empty() &&
               value.destination_directory.is_valid() &&
               (value.archive_name.empty() || safe_archive_name(value.archive_name)) &&
               static_cast<bool>(value.build_document);
    }
    void run_one(FilesVolumeArchivePlanningTicket ticket, FilesVolumeArchivePlanningRequest value,
                 std::stop_token token) {
        FilesVolumeArchivePlanningReply result;
        result.request = ticket;
        auto fail = [&](UnavailableObservation detail) {
            result.state = token.stop_requested() ? FilesVolumeArchivePlanningState::Cancelled
                                                   : FilesVolumeArchivePlanningState::Failed;
            result.error = std::move(detail);
            emit(std::move(result));
        };
        try {
            auto document = value.build_document(token);
            if (!document.value()) {
                fail(*document.unavailable_detail());
                return;
            }
            auto complete = std::move(document).take_value();
            complete.consistency = value.consistency;
            if (complete.source.identity != value.source.identity ||
                complete.source.mount_point != value.source.mount_point ||
                complete.source.filesystem != value.source.filesystem ||
                complete.source.native_mount_units != value.source.native_mount_units ||
                complete.source.archive.support != value.source.archive.support ||
                complete.source.archive.snapshot_available != value.source.archive.snapshot_available) {
                fail(error(AdapterError::UnqualifiedIdentity, "volume_archive.plan.identity_revalidation",
                            ObservationState::Changed));
                return;
            }
            auto destination = probe_archive_destination(value.destination_directory,
                                                         value.archive_name.empty()
                                                             ? FilesVolumeArchiveReview::default_archive_name(value.source.name)
                                                             : value.archive_name);
            if (!destination.value()) {
                fail(*destination.unavailable_detail());
                return;
            }
            auto review = FilesVolumeArchiveReview::create(std::move(complete), value.destination_directory,
                                                           std::move(value.archive_name), *destination.value());
            if (!review.value()) {
                fail(*review.unavailable_detail());
                return;
            }
            result.review = std::move(review).take_value();
            result.state = FilesVolumeArchivePlanningState::Ready;
            emit(std::move(result));
        } catch (...) {
            fail(error(AdapterError::PlanInvalid, "volume_archive.plan.worker_fault"));
        }
    }
    void run() {
        for (;;) {
            std::optional<std::pair<FilesVolumeArchivePlanningTicket, FilesVolumeArchivePlanningRequest>> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) {
                    pending.reset();
                    return;
                }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                stop = std::stop_source{};
                signalled = false;
            }
            run_one(std::move(job->first), std::move(job->second), stop.get_token());
        }
    }
};

FilesVolumeArchivePlanningService::FilesVolumeArchivePlanningService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesVolumeArchivePlanningService::~FilesVolumeArchivePlanningService() = default;
FilesVolumeArchivePlanningSubmitted FilesVolumeArchivePlanningService::request(
    FilesVolumeArchivePlanningRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return {FilesVolumeArchivePlanningSubmission::Stopped, {}};
    if (!State::valid(request))
        return {FilesVolumeArchivePlanningSubmission::Invalid, {}};
    if (state_->busy || state_->pending)
        return {FilesVolumeArchivePlanningSubmission::Busy, {}};
    auto ticket = FilesVolumeArchivePlanningTicket(state_->next++);
    state_->latest = ticket;
    state_->pending = std::make_pair(ticket, std::move(request));
    state_->condition.notify_one();
    return {FilesVolumeArchivePlanningSubmission::Accepted, ticket};
}
bool FilesVolumeArchivePlanningService::cancel(const FilesVolumeArchivePlanningTicket &request) {
    std::lock_guard lock(state_->mutex);
    if (!request.valid() || request != state_->latest || (!state_->busy && !state_->pending))
        return false;
    if (state_->pending && !state_->busy) {
        state_->pending.reset();
        FilesVolumeArchivePlanningReply cancelled;
        cancelled.request = request;
        cancelled.state = FilesVolumeArchivePlanningState::Cancelled;
        cancelled.error = error(AdapterError::InterruptedRead, "volume_archive.plan.cancelled",
                                ObservationState::Unknown);
        state_->reply = std::move(cancelled);
        state_->signal();
        return true;
    }
    state_->stop.request_stop();
    state_->condition.notify_all();
    return true;
}
std::optional<FilesVolumeArchivePlanningReply> FilesVolumeArchivePlanningService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesVolumeArchivePlanningService::request_stop() noexcept { state_->request_stop(); }
void FilesVolumeArchivePlanningService::join() { state_->join(); }
bool FilesVolumeArchivePlanningService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesVolumeArchivePlanningService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

struct FilesVolumeArchiveService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<FilesVolumeArchiveTicket, FilesVolumeArchiveRequest>> pending;
    std::optional<FilesVolumeArchiveReply> reply;
    FilesVolumeArchiveTicket latest, cancelled;
    std::uint64_t next = 1;
    std::stop_source stop;
    bool stopping = false, busy = false, signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        stop.request_stop();
        condition.notify_all();
    }
    void join() {
        if (worker.joinable())
            worker.join();
    }
    void signal() {
        if (signalled || !wake)
            return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    void emit(FilesVolumeArchiveReply value) {
        std::lock_guard lock(mutex);
        if (stopping || value.request != latest)
            return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    static bool valid(const FilesVolumeArchiveRequest &value) {
        return value.source.identity.size() != 0 && value.source.mount_point.size() != 0 &&
               value.destination_directory.is_valid() && safe_archive_name(value.archive_name) &&
               value.transaction_id != 0 && static_cast<bool>(value.build_document) && value.journal_root &&
               value.journal_root->is_valid() && static_cast<bool>(value.open_journal);
    }
    void run_one(FilesVolumeArchiveTicket ticket, FilesVolumeArchiveRequest value, std::stop_token token) {
        FilesVolumeArchiveReply result;
        result.request = ticket;
        std::shared_ptr<ArchiveJournalBinding> journal;
        auto fail = [&](UnavailableObservation detail) {
            if (journal && !journal->journal->records().empty()) {
                const auto current = journal->journal->state(0);
                if (current && !ck::files::operation_journal_terminal(*current)) {
                    const auto terminal = journal->append(
                        token.stop_requested() ? ck::files::OperationJournalState::Cancelled
                                               : ck::files::OperationJournalState::Failed);
                    if (!terminal.value())
                        detail = *terminal.unavailable_detail();
                }
            }
            result.state = token.stop_requested() ? FilesVolumeArchiveState::Cancelled : FilesVolumeArchiveState::Failed;
            result.error = std::move(detail);
            emit(std::move(result));
        };
        try {
            auto journal_id = value.journal_id;
            const auto destination_native_path = native_path_bytes(value.destination_directory);
            if (!destination_native_path) {
                fail(error(AdapterError::UnrepresentableName, "volume_archive.destination_evidence",
                            ObservationState::Unsupported));
                return;
            }
            const auto staging_name = archive_staging_name(value.destination_directory, value.archive_name,
                                                            value.transaction_id);
            if (!staging_name) {
                fail(error(AdapterError::UnrepresentableName, "volume_archive.staging_name",
                            ObservationState::Unsupported));
                return;
            }
            const auto staging_address = value.destination_directory.child_address(*staging_name);
            if (!staging_address.value() || !native_path_bytes(*staging_address.value())) {
                fail(staging_address.unavailable_detail()
                         ? *staging_address.unavailable_detail()
                         : error(AdapterError::UnrepresentableName, "volume_archive.staging_address",
                                 ObservationState::Unsupported));
                return;
            }
            auto journal_name = archive_journal_name(*value.journal_root, journal_id);
            if (!journal_name) {
                fail(error(AdapterError::UnrepresentableName, "volume_archive.journal_name",
                            ObservationState::Unsupported));
                return;
            }
            auto opened_journal = value.open_journal(*value.journal_root, *journal_name, true);
            if (!opened_journal.value()) {
                fail(*opened_journal.unavailable_detail());
                return;
            }
            auto created_journal = ck::files::OperationJournal::create(
                ck::files::FileOperation::Copy, journal_id, value.journal_limits);
            if (!created_journal.value()) {
                fail(*created_journal.unavailable_detail());
                return;
            }
            std::vector<std::byte> evidence;
            append_le<std::uint64_t>(evidence, value.transaction_id);
            append_string(evidence, value.source.identity);
            append_string(evidence, value.source.mount_point);
            append_string(evidence, value.archive_name);
            append_native_path_evidence(evidence, value.destination_directory);
            const auto staging_flags_offset = evidence.size();
            append_le<std::uint8_t>(evidence, kStagingPathEvidence);
            append_native_path_evidence(evidence, *staging_address.value());
            journal = std::make_shared<ArchiveJournalBinding>(
                std::move(created_journal).take_value(),
                std::shared_ptr<ck::files::OperationJournalFile>(std::move(opened_journal).take_value()),
                std::move(evidence), staging_flags_offset);
            auto planned = journal->append(ck::files::OperationJournalState::Planned);
            if (!planned.value()) {
                fail(*planned.unavailable_detail());
                return;
            }
            auto document = value.build_document(token);
            if (!document.value()) {
                fail(*document.unavailable_detail());
                return;
            }
            auto complete = std::move(document).take_value();
            complete.consistency = value.consistency;
            if (complete.source.identity != value.source.identity ||
                complete.source.mount_point != value.source.mount_point ||
                complete.source.filesystem != value.source.filesystem ||
                complete.source.native_mount_units != value.source.native_mount_units ||
                complete.source.archive.support != value.source.archive.support ||
                complete.source.archive.snapshot_available != value.source.archive.snapshot_available) {
                fail(error(AdapterError::UnqualifiedIdentity, "volume_archive.identity_revalidation",
                            ObservationState::Changed));
                return;
            }
            auto prepared = journal->append(ck::files::OperationJournalState::Prepared);
            if (!prepared.value()) {
                fail(*prepared.unavailable_detail());
                return;
            }
            auto destination = value.destination_directory.host_path();
            if (!destination.value()) {
                fail(*destination.unavailable_detail());
                return;
            }
            FilesVolumeArchiveWriteRequest write_request{
                std::move(complete), *destination.value(), std::move(value.archive_name), value.transaction_id,
                [journal](ck::files::OperationJournalState state) { return journal->append(state); },
                [journal](ck::files::OperationJournalState state, std::span<const std::byte> digest) {
                    return journal->append_staging_digest(state, digest);
                }};
            auto written = write_volume_archive(write_request, token);
            if (!written.value()) {
                fail(*written.unavailable_detail());
                return;
            }
            result.result = std::move(written).take_value();
            result.state = result.result->publication == FilesVolumeArchivePublication::Durable
                               ? FilesVolumeArchiveState::Complete
                               : FilesVolumeArchiveState::Indeterminate;
            emit(std::move(result));
        } catch (...) {
            fail(error(AdapterError::PlanInvalid, "volume_archive.worker_fault"));
        }
    }
    void run() {
        for (;;) {
            std::optional<std::pair<FilesVolumeArchiveTicket, FilesVolumeArchiveRequest>> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) {
                    pending.reset();
                    return;
                }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                stop = std::stop_source{};
                signalled = false;
            }
            run_one(std::move(job->first), std::move(job->second), stop.get_token());
        }
    }
};

FilesVolumeArchiveService::FilesVolumeArchiveService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesVolumeArchiveService::~FilesVolumeArchiveService() = default;
FilesVolumeArchiveSubmitted FilesVolumeArchiveService::request(FilesVolumeArchiveRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return {FilesVolumeArchiveSubmission::Stopped, {}};
    if (!State::valid(request))
        return {FilesVolumeArchiveSubmission::Invalid, {}};
    if (state_->busy || state_->pending)
        return {FilesVolumeArchiveSubmission::Busy, {}};
    auto ticket = FilesVolumeArchiveTicket(state_->next++);
    state_->latest = ticket;
    state_->pending = std::make_pair(ticket, std::move(request));
    state_->condition.notify_one();
    return {FilesVolumeArchiveSubmission::Accepted, ticket};
}
bool FilesVolumeArchiveService::cancel(const FilesVolumeArchiveTicket &request) {
    std::lock_guard lock(state_->mutex);
    if (!request.valid() || request != state_->latest || (!state_->busy && !state_->pending))
        return false;
    state_->cancelled = request;
    if (state_->pending && !state_->busy) {
        state_->pending.reset();
        FilesVolumeArchiveReply cancelled;
        cancelled.request = request;
        cancelled.state = FilesVolumeArchiveState::Cancelled;
        cancelled.error = error(AdapterError::InterruptedRead, "volume_archive.cancelled", ObservationState::Unknown);
        state_->reply = std::move(cancelled);
        state_->signal();
        return true;
    }
    state_->stop.request_stop();
    state_->condition.notify_all();
    return true;
}
std::optional<FilesVolumeArchiveReply> FilesVolumeArchiveService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesVolumeArchiveService::request_stop() noexcept { state_->request_stop(); }
void FilesVolumeArchiveService::join() { state_->join(); }
bool FilesVolumeArchiveService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesVolumeArchiveService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

} // namespace ck::vision
