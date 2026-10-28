// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
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
constexpr std::uint32_t kVersion = 1;
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

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t &result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

bool safe_relative_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find('\0') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find_first_of("/\\", start);
        const auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string_view::npos)
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

    std::set<std::string> paths;
    std::vector<EncodedEntry> encoded;
    encoded.reserve(document.entries.size());
    data_size = data_bytes = 0;
    for (const auto &entry : document.entries) {
        if (stop_token.stop_requested())
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InterruptedRead, "volume_archive.cancelled"));
        if (!safe_relative_path(entry.path) || !paths.insert(entry.path).second)
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unsupported, error(AdapterError::UnrepresentableName, "volume_archive.entry_path"));
        if (entry.kind == FilesVolumeArchiveEntryKind::HardLink && entry.hard_link_group == 0)
            return Observation<std::vector<EncodedEntry>>::unavailable(
                ObservationState::Unknown, error(AdapterError::InvalidIdentifier, "volume_archive.hard_link"));
        if (entry.kind != FilesVolumeArchiveEntryKind::RegularFile &&
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
    for (int i = 0; i != 7; ++i)
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
                             UnavailableObservation &failure_detail) {
    std::error_code ec;
    std::filesystem::create_hard_link(temporary, published, ec);
    if (ec) {
        failure_detail = filesystem_error("volume_archive.publish", ec.value());
        return false;
    }
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

} // namespace

Observation<FilesVolumeArchiveWriteResult> write_volume_archive(const FilesVolumeArchiveWriteRequest &request,
                                                                std::stop_token stop_token) {
    if (!valid_request(request))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(
            ObservationState::Unsupported, error(AdapterError::PlanInvalid, "volume_archive.request"));
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
            if (!verify || std::memcmp(bytes_check.data(), extent.bytes->data(), bytes_check.size()) != 0) {
                verify.close();
                std::filesystem::remove(temp, ec);
                return Observation<FilesVolumeArchiveWriteResult>::unavailable(
                    ObservationState::Changed, error(AdapterError::MetadataChanged, "volume_archive.verify_extent"));
            }
        }
    verify.close();
    if (!publish_without_replace(temp, published, failure_detail)) {
        std::filesystem::remove(temp, ec);
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(failure_detail.state, failure_detail.error);
    }
    if (!durable_directory(request.destination_directory, failure_detail))
        return Observation<FilesVolumeArchiveWriteResult>::unavailable(failure_detail.state, failure_detail.error);
    return Observation<FilesVolumeArchiveWriteResult>::available(
        {published, logical_size, data_bytes, encoded.size()});
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
               value.transaction_id != 0 && static_cast<bool>(value.build_document);
    }
    void run_one(FilesVolumeArchiveTicket ticket, FilesVolumeArchiveRequest value, std::stop_token token) {
        FilesVolumeArchiveReply result;
        result.request = ticket;
        auto fail = [&](UnavailableObservation detail) {
            result.state = token.stop_requested() ? FilesVolumeArchiveState::Cancelled : FilesVolumeArchiveState::Failed;
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
            if (complete.source.identity != value.source.identity || complete.source.mount_point != value.source.mount_point) {
                fail(error(AdapterError::UnqualifiedIdentity, "volume_archive.identity_revalidation",
                            ObservationState::Changed));
                return;
            }
            auto destination = value.destination_directory.host_path();
            if (!destination.value()) {
                fail(*destination.unavailable_detail());
                return;
            }
            FilesVolumeArchiveWriteRequest write_request{
                std::move(complete), *destination.value(), std::move(value.archive_name), value.transaction_id};
            auto written = write_volume_archive(write_request, token);
            if (!written.value()) {
                fail(*written.unavailable_detail());
                return;
            }
            result.state = FilesVolumeArchiveState::Complete;
            result.result = std::move(written).take_value();
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
