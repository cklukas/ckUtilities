// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive.hpp"
#include "files_recovery_finalization.hpp"
#include "native_filesystem.hpp"
#include "native_operation_journal_id.hpp"
#include "native_archive_write_job.hpp"
#include "private_journal_scope.hpp"
#include "platform/prepared_journal_root_bridge.hpp"
#include "sha256.hpp"
#include "unicode_scalar.hpp"
#if defined(_WIN32)
#include "windows_name.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <tuple>
#include <utility>
#include <string_view>
#include <variant>

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


#if defined(CK_FS_ARCHIVE_READBACK_FAILURE_TEST)
namespace ck::vision::detail {
// Failure-only worker seam immediately before the final authoritative read.
// Neither hook is called by a native close/finalizer/destructor.
void archive_before_final_root_read(ck::files::PreparedJournalRoot&) noexcept;
void archive_final_root_read_observed(bool found,bool pending) noexcept;
}
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
std::optional<ck::files::NativeName> windows_name_from_utf8(std::string_view text);

constexpr std::string_view archive_journal_prefix = ".ck-volume-archive-";

bool is_archive_journal_name(const ck::files::NativeName &name) {
    const auto matches = [](const auto &units) {
        if (units.size() != archive_journal_prefix.size() + 32 ||
            !std::equal(archive_journal_prefix.begin(), archive_journal_prefix.end(), units.begin()))
            return false;
        return std::all_of(units.begin() + archive_journal_prefix.size(), units.end(), [](auto unit) {
            return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'f');
        });
    };
    if (const auto *bytes = name.posix_bytes())
        return matches(*bytes);
    const auto *units = name.windows_units();
    return units && matches(*units);
}

// Opening the route here only checks that it still names the held object.
// The resulting cursor is never used for archive publication.
Observation<bool> revalidate_archive_destination(const ck::files::NativeAbsolutePath &path,
                                                  const ck::files::DirectoryCursor &held,
                                                  const char *changed_operation =
                                                      "volume_archive.review.destination_changed") {
    const auto scope = held.source_scope();
    if (!scope.value())
        return Observation<bool>::unavailable(scope.state(), scope.unavailable_detail()->error);
    ck::files::NativeFileSystem filesystem(*scope.value());
    const auto changed = [changed_operation] {
        return Observation<bool>::unavailable(ObservationState::Changed,
            error(AdapterError::PlanDecisionStale, changed_operation));
    };
    const auto route = filesystem.open_native_directory(path);
    if (!route.value()) {
        const auto &detail = *route.unavailable_detail();
        if (detail.state == ObservationState::Gone ||
            (detail.error.domain == ErrorDomain::Adapter &&
             detail.error.code == static_cast<std::int64_t>(AdapterError::FinalLinkRejected)))
            return changed();
        return Observation<bool>::unavailable(detail.state, detail.error);
    }
    const auto original = held.inspect_basic();
    if (!original.value())
        return Observation<bool>::unavailable(original.state(), original.unavailable_detail()->error);
    const auto current = (*route.value())->inspect_basic();
    if (!current.value())
        return Observation<bool>::unavailable(current.state(), current.unavailable_detail()->error);
    const auto &a = *original.value();
    const auto &b = *current.value();
    if (!a.identity.value() || !b.identity.value() || !a.mount_id.value() || !b.mount_id.value() ||
        ck::files::compare_identity(*a.identity.value(), *b.identity.value()) != ck::files::IdentityComparison::Same ||
        *a.mount_id.value() != *b.mount_id.value())
        return changed();
    return Observation<bool>::available(true);
}

bool valid_archive_recovery_scan(const ck::files::NativeAbsolutePath &root,
                                 const FilesVolumeArchiveRecoveryLimits &limits) {
    return root.is_valid() && limits.max_journals != 0 && limits.max_journals <= 16 * 1024 &&
           limits.journal.max_items != 0 && limits.journal.max_records != 0 && limits.codec.max_bytes != 0 &&
           limits.max_scan_batches != 0 && limits.max_scan_batches <= 4096;
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

Observation<FilesVolumeArchiveJournalEvidence> decode_legacy_archive_journal_evidence(
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

// Native ARW2 envelope. Component values remain evidence only: this decoder
// performs no lookup, reopen, discard or publication, including for Named.
Observation<FilesVolumeArchiveJournalEvidence> decode_archive_journal_evidence(std::span<const std::byte> bytes) {
    using R=Observation<FilesVolumeArchiveJournalEvidence>;
    if(bytes.size()<4||bytes[0]!=std::byte{'A'}||bytes[1]!=std::byte{'R'}||bytes[2]!=std::byte{'W'})
        return decode_legacy_archive_journal_evidence(bytes);
    const auto bad=[](){return R::unavailable(ObservationState::Unknown,error(AdapterError::PlanInvalid,"volume_archive.native_evidence"));};
    if(bytes[3]!=std::byte{2})return bad();
    std::size_t offset=4;
    auto component=[&]()->std::optional<ck::files::NativeName>{
        std::uint8_t encoding=0;std::uint64_t count=0;
        if(!read_le(bytes,offset,encoding)||encoding>1||!read_le(bytes,offset,count)||!count||count>bytes.size()-offset)return {};
        auto data=bytes.subspan(offset,static_cast<std::size_t>(count));offset+=static_cast<std::size_t>(count);
        if(!encoding)return ck::files::NativeName::from_posix(std::string(reinterpret_cast<const char*>(data.data()),data.size()));
        if(count%2)return {};
        std::u16string units;units.reserve(data.size()/2);
        for(std::size_t i=0;i<data.size();i+=2)units.push_back(static_cast<char16_t>(std::to_integer<unsigned char>(data[i])|(std::to_integer<unsigned char>(data[i+1])<<8)));
        return ck::files::NativeName::from_windows(std::move(units));
    };
    const auto destination=component();std::uint8_t ns=0,has_name=0,verified=0;std::uint64_t size=0,application_size=0;
    if(!destination||!read_le(bytes,offset,ns)||ns>static_cast<std::uint8_t>(ck::files::StagingNamespace::Anonymous)||!read_le(bytes,offset,has_name)||has_name>1)return bad();
    std::optional<ck::files::NativeName> staging;if(has_name){staging=component();if(!staging)return bad();}
    const auto space=static_cast<ck::files::StagingNamespace>(ns);
    if((space==ck::files::StagingNamespace::Anonymous&&staging)||(space==ck::files::StagingNamespace::Named&&!staging)||
        (space==ck::files::StagingNamespace::Unknown&&staging)||!read_le(bytes,offset,size)||!read_le(bytes,offset,verified)||verified>1||bytes.size()-offset<32)return bad();
    std::array<std::byte,32> digest{};std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(offset),32,digest.begin());offset+=32;
    if(!read_le(bytes,offset,application_size)||application_size>1024*1024||application_size!=bytes.size()-offset)return bad();
    auto application=decode_legacy_archive_journal_evidence(bytes.subspan(offset));if(!application.value())return application;
    auto result=std::move(application).take_value();
    if(!result.staging_native_path.empty()||result.staging_encoding||result.staging_digest)return bad();
    // Bind the application label to exact native output spelling; escaped UI
    // text is never parsed as a component or used for I/O.
    std::optional<ck::files::NativeName> expected;
    if(destination->encoding()==ck::files::NameEncoding::PosixBytes)expected=ck::files::NativeName::from_posix(result.archive_name);
    else expected=windows_name_from_utf8(result.archive_name);
    if(!expected||*expected!=*destination)return bad();
    result.staging_namespace=space;result.staging_component=std::move(staging);result.archive_logical_size=size;
    if(verified)result.staging_digest=digest;
    return R::available(std::move(result));
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

std::optional<ck::files::NativeName> windows_name_from_utf8(std::string_view text) {
    std::u16string units;
    units.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = ck::files::detail::decode_utf8_scalar(text.substr(offset));
        if (scalar.units == 0)
            return std::nullopt;
        const auto encoded = ck::files::detail::encode_utf16_scalar(scalar.value);
        if (encoded.units == 0)
            return std::nullopt;
        units.append(encoded.data.data(), encoded.units);
        offset += scalar.units;
    }
    return ck::files::NativeName::from_windows(std::move(units));
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

constexpr std::uint64_t kMaterialSparseHoleBytes = 64 * 1024;

bool has_material_sparse_hole(const std::vector<EncodedEntry> &entries) {
    for (const auto &encoded : entries) {
        const auto &entry = *encoded.source;
        if (entry.kind != FilesVolumeArchiveEntryKind::RegularFile)
            continue;
        std::uint64_t previous_end = 0;
        for (const auto &extent : entry.extents) {
            if (extent.offset >= previous_end && extent.offset - previous_end >= kMaterialSparseHoleBytes)
                return true;
            previous_end = extent.offset + extent.bytes.size();
        }
        if (entry.logical_size >= previous_end && entry.logical_size - previous_end >= kMaterialSparseHoleBytes)
            return true;
    }
    return false;
}

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

#if defined(_WIN32)
std::optional<std::wstring> archive_windows_path(const std::filesystem::path &path) {
    return ck::files::windows_extended_path(path.wstring());
}

struct WindowsArchivePathStatus final {
    bool exists = false;
    bool directory = false;
    bool reparse_point = false;
    DWORD error = ERROR_SUCCESS;
};

WindowsArchivePathStatus archive_path_status(const std::filesystem::path &path) {
    const auto native_path = archive_windows_path(path);
    if (!native_path)
        return {.error = ERROR_INVALID_NAME};
    const DWORD attributes = GetFileAttributesW(native_path->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return {.error = error};
    }
    return {.exists = true,
            .directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            .reparse_point = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0};
}

#endif

bool valid_request(const FilesVolumeArchiveWriteRequest &request) {
    return request.destination_directory.is_absolute() && safe_archive_name(request.archive_name) &&
           request.transaction_id != 0;
}

std::optional<ck::files::NativeName> archive_journal_name(ck::files::NameEncoding encoding,
                                                          const ck::files::OperationJournalId &id) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text = ".ck-volume-archive-";
    for (const auto byte : id.bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0FU]);
    }
    if (encoding == ck::files::NameEncoding::PosixBytes)
        return ck::files::NativeName::from_posix(std::move(text));
    if (encoding == ck::files::NameEncoding::WindowsUtf16) {
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
        return windows_name_from_utf8(text);
    }
    return std::nullopt;
}


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
                (evidence->staging_namespace!=ck::files::StagingNamespace::Unknown&&evidence->staging_namespace!=current.staging_namespace) ||
                (evidence->staging_component&&evidence->staging_component!=current.staging_component) ||
                (evidence->archive_logical_size&&evidence->archive_logical_size!=current.archive_logical_size) ||
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
    const ck::files::NativeAbsolutePath &private_root, FilesVolumeArchiveRecoveryLimits limits,
    std::stop_token stop_token) {
    using Result = Observation<FilesVolumeArchiveRecoveryScan>;
    if (!valid_archive_recovery_scan(private_root, limits))
        return Result::unavailable(ObservationState::Failed,
            error(AdapterError::InvalidLimit, "volume_archive.recovery_scan_limits"));
    const auto cancelled = [] {
        return Result::unavailable(ObservationState::Unknown,
            error(AdapterError::InterruptedRead, "volume_archive.recovery_cancelled"));
    };
    if (stop_token.stop_requested())
        return cancelled();

    auto opened = ck::files::open_native_operation_journal_directory(private_root);
    if (!opened.value())
        return Result::unavailable(opened.state(), opened.unavailable_detail()->error);
    auto cursor = std::move(opened).take_value();
    return detail::finish_recovery_observation<FilesVolumeArchiveRecoveryScan>(
        [&]() -> Result {
            FilesVolumeArchiveRecoveryScan result;
            auto remaining_batches = limits.max_scan_batches;
            for (;;) {
                if (remaining_batches == 0)
                    return Result::unavailable(ObservationState::Unknown,
                        error(AdapterError::InventoryLimit, "volume_archive.recovery_scan_batches"));
                --remaining_batches;
                const auto batch = cursor->read(
                    std::min(limits.max_journals, ck::files::DirectoryCursor::max_batch_size), stop_token);
                if (batch.error)
                    return Result::unavailable(batch.error->state, batch.error->error);
                if (batch.state == ck::files::DirectoryReadState::Cancelled || stop_token.stop_requested())
                    return cancelled();

                for (const auto &name : batch.names) {
                    if (stop_token.stop_requested())
                        return cancelled();
                    if (!is_archive_journal_name(name))
                        continue;
                    if (result.journals.size() >= limits.max_journals)
                        return Result::unavailable(ObservationState::Unknown,
                            error(AdapterError::InventoryLimit, "volume_archive.recovery_count"));
                    FilesVolumeArchiveRecoveryItem item;
                    item.native_name = name;
                    item.display_name = name.display_name();
                    auto file = ck::files::open_held_operation_journal_file(*cursor, name);
                    if (!file.value()) {
                        item.error = *file.unavailable_detail();
                    } else {
                        auto report = detail::finish_recovery_observation<FilesVolumeArchiveRecoveryReport>(
                            [&] { return inspect_volume_archive_recovery(**file.value(), limits.journal, limits.codec); },
                            [&] { return (*file.value())->finish_io(); }, "volume_archive.recovery_finish_io");
                        if (report.value())
                            item.report = std::move(report).take_value();
                        else
                            item.error = *report.unavailable_detail();
                    }
                    result.requires_reconciliation = result.requires_reconciliation ||
                        item.error || (item.report && item.report->operation.requires_reconciliation);
                    result.journals.push_back(std::move(item));
                }
                if (batch.state == ck::files::DirectoryReadState::End)
                    break;
                if (batch.state != ck::files::DirectoryReadState::More)
                    return Result::unavailable(ObservationState::Failed,
                        error(AdapterError::InvalidNativeResult, "volume_archive.recovery_enumerate"));
            }
            const auto route = revalidate_archive_destination(private_root, *cursor,
                                                               "volume_archive.recovery_root_changed");
            if (!route.value())
                return Result::unavailable(route.state(), route.unavailable_detail()->error);
            return stop_token.stop_requested() ? cancelled() : Result::available(std::move(result));
        }, [&] { return ck::files::finish_operation_journal_directory(*cursor); },
        "volume_archive.recovery_directory_finish_io");
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
        return valid_archive_recovery_scan(root, limits);
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
            auto scan = discover_volume_archive_recovery(root, limits, stop.get_token());
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

Observation<bool> cleanup_volume_archive_recovery(
    const ck::files::NativeAbsolutePath &, const ck::files::NativeName &,
    const ck::files::OperationJournalId &, FilesVolumeArchiveRecoveryLimits,
    FilesVolumeArchiveRecoveryCleanupLimits) {
    // Restart metadata and matching content cannot identify the original
    // created object. Preserve both artifact and journal; do not reopen them
    // or synthesize ownership from a newly observed identity.
    return Observation<bool>::unavailable(ObservationState::Unknown,
        error(AdapterError::UnqualifiedIdentity, "volume_archive.cleanup_live_creator_ownership_required"));
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

namespace {

Observation<bool> revalidate_archive_source(const FilesVolumeArchiveSourceRevalidation *source,
                                            std::stop_token token) {
    if (!source)
        return Observation<bool>::available(true);
    const auto checked = (*source)(token);
    if (checked.value() && !*checked.value())
        return Observation<bool>::unavailable(ObservationState::Changed,
            error(AdapterError::UnqualifiedIdentity, "volume_archive.source.mount_changed"));
    return checked;
}

Observation<FilesVolumeArchiveWriteResult> write_bound_volume_archive(
    const FilesVolumeArchiveWriteRequest &request, std::stop_token stop_token,
    const FilesVolumeArchiveSourceRevalidation *source) {
    using R=Observation<FilesVolumeArchiveWriteResult>;
    if(!valid_request(request))return R::unavailable(ObservationState::Unsupported,error(AdapterError::PlanInvalid,"volume_archive.request"));
    if(!request.destination_owner||!request.prepared_journal_root||!request.native_journal_name||request.append_journal||request.append_journal_evidence)
        return R::unavailable(ObservationState::Unsupported,error(AdapterError::UnqualifiedIdentity,"volume_archive.native_owner_and_journal_required"));
    if(!request.document.losses.empty()&&!request.document.losses_reviewed)
        return R::unavailable(ObservationState::Unknown,error(AdapterError::ProviderReviewRequired,"volume_archive.loss_consent"));
    if(std::any_of(request.document.losses.begin(),request.document.losses.end(),[](const auto& l){return l.hard_failure;}))
        return R::unavailable(ObservationState::Unsupported,error(AdapterError::ProviderReviewRequired,"volume_archive.hard_loss"));
    std::uint64_t data_size=0,data_bytes=0;
    auto enc=validate_and_encode(request.document,data_size,data_bytes,stop_token);
    if(!enc.value())return R::unavailable(enc.state(),enc.unavailable_detail()->error);
    auto encoded=std::move(enc).take_value();std::vector<std::byte> manifest;append_manifest(manifest,request.document,encoded);
    std::uint64_t data_offset=0,logical_size=0;
    if(!checked_add(kHeaderSize,manifest.size(),data_offset)||!checked_add(data_offset,data_size,logical_size))
        return R::unavailable(ObservationState::Unknown,error(AdapterError::InventoryLimit,"volume_archive.logical_size"));
    const auto address=ck::files::NativeAbsolutePath::from_host(request.destination_directory);
    if(!address.value())return R::unavailable(address.state(),address.unavailable_detail()->error);
    const auto final=address.value()->encoding()==ck::files::NameEncoding::PosixBytes?
        ck::files::NativeName::from_posix(request.archive_name):windows_name_from_utf8(request.archive_name);
    const auto hint=archive_staging_name(*address.value(),request.archive_name,request.transaction_id);
    if(!final||!hint)return R::unavailable(ObservationState::Unsupported,error(AdapterError::UnrepresentableName,"volume_archive.exact_names"));
    // The address records the reviewed route and can label a confirmed result.
    // Every native write and the publication itself use the held directory.
    auto output_address=address.value()->child_address(*final);
    if(!output_address.value())return R::unavailable(output_address.state(),output_address.unavailable_detail()->error);
    auto output=output_address.value()->host_path();if(!output.value())return R::unavailable(output.state(),output.unavailable_detail()->error);
    ck::files::NativeArchiveWriteRequest native{
        .destination=request.destination_owner, .journal_root=request.prepared_journal_root,
        .destination_name=*final, .staging_hint=*hint, .journal_name=*request.native_journal_name,
        .journal_id=request.native_journal_id, .extents={}, .application_evidence={}};
    native.logical_size=logical_size;native.journal_limits=request.journal_limits;
    // Native layout must demonstrate a hole for a material sparse archive.
    native.minimum_unallocated_bytes=has_material_sparse_hole(encoded)?1:0;
    append_le<std::uint64_t>(native.application_evidence,request.transaction_id);
    append_string(native.application_evidence,request.document.source.identity);
    append_string(native.application_evidence,request.document.source.mount_point);
    append_string(native.application_evidence,request.archive_name);
    append_native_path_evidence(native.application_evidence,*address.value());
    std::vector<std::byte> header;
    append_header(header,request.document.consistency==FilesVolumeArchiveConsistency::Snapshot?1U:0U,
        manifest.size(),data_offset,data_size,encoded.size());
    native.extents.push_back({0,std::move(header)});
    if(!manifest.empty())native.extents.push_back({kHeaderSize,std::move(manifest)});
    for(const auto& entry:encoded)for(const auto& extent:entry.extents)
        native.extents.push_back({data_offset+extent.archive_offset,*extent.bytes});
    if (!stop_token.stop_requested()) {
        const auto route = revalidate_archive_destination(*address.value(), *request.destination_owner);
        if (!route.value())
            return R::unavailable(route.state(), route.unavailable_detail()->error);
    }
    auto began=ck::files::NativeArchiveWriteJob::begin(std::move(native));
    if(!began.value())return R::unavailable(began.state(),began.unavailable_detail()->error);
    auto job = std::move(began).take_value();
    using Phase = ck::files::NativeArchivePhase;
    auto progress = job->progress();
    std::optional<UnavailableObservation> stopped_by_review;
    while (progress.phase != Phase::Complete && progress.phase != Phase::Cancelled &&
           progress.phase != Phase::NeedsReconciliation) {
        // Check before the durable intent and again after its flush. The
        // native publish is still relative to the held directory if the
        // requested route changes during the final system call.
        if (!stop_token.stop_requested() &&
            (progress.phase == Phase::CommitIntent || progress.phase == Phase::Publish)) {
            auto checked = revalidate_archive_source(source, stop_token);
            if (checked.value())
                checked = revalidate_archive_destination(*address.value(), *request.destination_owner);
            if (!checked.value()) {
                stopped_by_review = *checked.unavailable_detail();
                progress = job->finish_io();
                continue;
            }
        }
        progress = job->advance(stop_token);
        if (progress.phase == Phase::Drain && !progress.drained)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Finish the native writer before interpreting a route check or reporting
    // a result. In particular, a drain failure outranks a stale review.
    job.reset();
    std::optional<std::filesystem::path> verified_path;
    if (progress.published) {
        try {
            if (revalidate_archive_destination(*address.value(), *request.destination_owner).value())
                verified_path = std::move(output).take_value();
        } catch (...) {
            // The publication result survives inability to label its location.
        }
    }
    if (progress.phase == Phase::Complete)
        return R::available({std::move(verified_path),logical_size,data_bytes,encoded.size(),
            FilesVolumeArchivePublication::Durable,{},progress.image_verified,true});
    if (progress.published || progress.publication_uncertain)
        return R::available({std::move(verified_path),logical_size,data_bytes,encoded.size(),
            FilesVolumeArchivePublication::PublishedDurabilityUnknown,
            progress.failure ? progress.failure->operation.data() : "volume_archive.publication_unknown",
            progress.image_verified,progress.published});
    if (progress.failure)
        return R::unavailable(progress.failure->state,{progress.failure->domain,progress.failure->code,
                                                      progress.failure->operation.data()});
    if (stopped_by_review)
        return R::unavailable(stopped_by_review->state, std::move(stopped_by_review->error));
    return R::unavailable(ObservationState::Unknown,error(AdapterError::InterruptedRead,"volume_archive.cancelled"));
}

} // namespace

Observation<FilesVolumeArchiveWriteResult> write_volume_archive(const FilesVolumeArchiveWriteRequest &request,
                                                                std::stop_token stop_token) {
    return write_bound_volume_archive(request, stop_token, nullptr);
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

using ArchiveDestination = std::shared_ptr<const ck::files::DirectoryCursor>;

struct ArchiveBindings {
    ArchiveDestination destination;
    FilesVolumeArchiveSourceRevalidation source;
};

Observation<FilesVolumeArchiveDestinationState> probe_archive_destination(
    const ck::files::DirectoryCursor &directory, ck::files::NameEncoding encoding, std::string_view archive_name) {
    const auto name = encoding == ck::files::NameEncoding::WindowsUtf16
        ? windows_name_from_utf8(archive_name) : ck::files::NativeName::from_posix(std::string(archive_name));
    if (!name)
        return review_rejected<FilesVolumeArchiveDestinationState>(ObservationState::Unsupported,
            AdapterError::UnrepresentableName, "volume_archive.plan.name");
    const auto lookup = directory.lookup_child(*name, ck::files::ChildLookupMode::NativeDefault);
    if (!lookup.value())
        return Observation<FilesVolumeArchiveDestinationState>::unavailable(
            lookup.state(), lookup.unavailable_detail()->error);
    switch (lookup.value()->state()) {
    case ck::files::ChildLookupState::Found:
        return Observation<FilesVolumeArchiveDestinationState>::available(FilesVolumeArchiveDestinationState::Conflict);
    case ck::files::ChildLookupState::Missing:
        return Observation<FilesVolumeArchiveDestinationState>::available(FilesVolumeArchiveDestinationState::Ready);
    case ck::files::ChildLookupState::Unknown:
        break;
    }
    return review_rejected<FilesVolumeArchiveDestinationState>(ObservationState::Unknown,
        AdapterError::UnqualifiedIdentity, "volume_archive.plan.destination_name_unknown");
}

bool archive_source_matches(const FilesVolumeRecord &expected, const FilesVolumeRecord &observed) {
    const auto &a = expected.archive.root;
    const auto &b = observed.archive.root;
    if (a.has_value() != b.has_value() ||
        (a && (a->mount_id != b->mount_id ||
               ck::files::compare_identity(a->identity, b->identity) != ck::files::IdentityComparison::Same)))
        return false;
    return expected.identity == observed.identity && expected.mount_point == observed.mount_point &&
           expected.filesystem == observed.filesystem && expected.native_mount_units == observed.native_mount_units &&
           expected.archive.support == observed.archive.support &&
           expected.archive.snapshot_available == observed.archive.snapshot_available;
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

struct FilesVolumeArchiveService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    using PlanJob = std::pair<FilesVolumeArchivePlanningTicket, FilesVolumeArchivePlanningRequest>;
    using WriteJob = std::pair<FilesVolumeArchiveTicket, FilesVolumeArchiveRequest>;
    struct ReviewedJob {
        FilesVolumeArchiveTicket ticket;
        FilesVolumeArchiveApproval approval;
        ck::files::NativeAbsolutePath journal_root;
    };
    using Job = std::variant<PlanJob, WriteJob, ReviewedJob>;
    std::optional<Job> pending;
    std::optional<FilesVolumeArchivePlanningReply> plan_reply;
    std::optional<FilesVolumeArchiveReply> reply;
    FilesVolumeArchiveReview::Pointer ready_review;
    FilesVolumeArchivePlanningTicket latest_plan;
    FilesVolumeArchiveTicket latest;
    std::uint64_t next = 1;
    std::stop_source stop;
    bool stopping = false, busy = false, signalled = false, discard_review = false;
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
    bool reserved() const {
        return busy || pending || ready_review || discard_review;
    }
    bool finish_plan(FilesVolumeArchivePlanningReply result) {
        std::lock_guard lock(mutex);
        if (stopping)
            return false;
        if (stop.stop_requested()) {
            result.state = FilesVolumeArchivePlanningState::Cancelled;
            result.review.reset();
        }
        ready_review = result.review;
        plan_reply = std::move(result);
        busy = false;
        signal();
        return static_cast<bool>(ready_review);
    }
    ArchiveBindings run_plan(PlanJob job, std::stop_token token) {
        auto &[ticket, value] = job;
        FilesVolumeArchivePlanningReply result;
        result.request = ticket;
        auto fail = [&](UnavailableObservation detail) -> ArchiveBindings {
            result.error = std::move(detail);
            finish_plan(std::move(result));
            return {};
        };
        try {
            if (token.stop_requested())
                return fail(error(AdapterError::InterruptedRead, "volume_archive.plan.cancelled",
                                  ObservationState::Unknown));
            ck::files::NativeFileSystem filesystem;
            auto opened = filesystem.open_native_directory(value.destination_directory);
            if (!opened.value())
                return fail(*opened.unavailable_detail());
            auto destination = ArchiveDestination(std::move(opened).take_value());

            auto captured = value.capture_source(token);
            if (!captured.value())
                return fail(*captured.unavailable_detail());
            auto source = std::move(captured).take_value();
            source.document.consistency = value.consistency;
            if (!source.revalidate || !archive_source_matches(value.source, source.document.source))
                return fail(error(AdapterError::UnqualifiedIdentity, "volume_archive.plan.identity_revalidation",
                                  ObservationState::Changed));

            const auto route = revalidate_archive_destination(value.destination_directory, *destination);
            if (!route.value())
                return fail(*route.unavailable_detail());
            if (value.archive_name.empty())
                value.archive_name = FilesVolumeArchiveReview::default_archive_name(value.source.name);
            const auto conflict = probe_archive_destination(*destination, value.destination_directory.encoding(),
                                                            value.archive_name);
            if (!conflict.value())
                return fail(*conflict.unavailable_detail());
            auto review = FilesVolumeArchiveReview::create(std::move(source.document), std::move(value.destination_directory),
                                                           std::move(value.archive_name), *conflict.value());
            if (!review.value())
                return fail(*review.unavailable_detail());
            result.review = std::move(review).take_value();
            result.state = FilesVolumeArchivePlanningState::Ready;
            if (!finish_plan(std::move(result)))
                return {};
            return {std::move(destination), std::move(source.revalidate)};
        } catch (...) {
            result.state = FilesVolumeArchivePlanningState::WorkerFault;
            finish_plan(std::move(result));
            return {};
        }
    }
    static bool valid(const FilesVolumeArchiveRequest &value) {
        return value.source.identity.size() != 0 && value.source.mount_point.size() != 0 &&
               value.destination_directory.is_valid() && safe_archive_name(value.archive_name) &&
               value.transaction_id != 0 && static_cast<bool>(value.build_document) && (value.prepared_journal_root || (value.journal_root && value.journal_root->is_valid()));
    }
    void run_one(FilesVolumeArchiveTicket ticket, FilesVolumeArchiveRequest value, std::stop_token token,
                 ArchiveBindings bindings = {}) {
        auto &destination = bindings.destination;
        FilesVolumeArchiveReply result;result.request=ticket;
        ck::files::PreparedJournalRoot::Pointer root;bool owns_root=false,root_attempted=false;
        struct RootCloseObserver final:ck::files::MetadataCaptureFailureLatch {
            const ck::files::PreparedJournalRoot::Pointer& owner;
            explicit RootCloseObserver(const ck::files::PreparedJournalRoot::Pointer& p):owner(p){}
            void record_first(ck::files::MetadataCaptureCleanupError error)const noexcept override {
                if(owner)ck::files::detail::record_prepared_journal_consumer_failure(*owner,error.state,error.domain,error.code,error.operation);
            }
        } observer(root);
        ck::files::detail::MetadataCaptureCleanupScope cleanup_scope(&observer);
        std::optional<ck::files::NativeArchiveFailure> root_finish_failure;
        auto save_root_failure=[&](ObservationState state,ErrorDomain domain,std::int64_t code,std::string_view operation)noexcept{
            if(root_finish_failure)return;
            ck::files::NativeArchiveFailure error;error.state=state;error.domain=domain;error.code=code;
            std::copy_n(operation.data(),std::min(operation.size(),error.operation.size()-1),error.operation.begin());root_finish_failure=error;
        };
        auto close_root=[&]() {
            if(owns_root&&root&&!root_attempted){root_attempted=true;auto closed_root=root->finish_io();
                if(const auto* error=closed_root.unavailable_detail())save_root_failure(error->state,error->error.domain,error->error.code,error->error.operation);
                else if(!*closed_root.value())save_root_failure(ObservationState::Unknown,ErrorDomain::Adapter,static_cast<std::int64_t>(AdapterError::InvalidNativeResult),"volume_archive.root_finish_false");}
        };
        auto close_noexcept=[&]()noexcept{try{close_root();}catch(...){save_root_failure(ObservationState::Unknown,ErrorDomain::Adapter,
            static_cast<std::int64_t>(AdapterError::InvalidNativeResult),"volume_archive.root_finish_exception");}};
        auto read_root=[&]()noexcept->std::optional<ck::files::NativeArchiveFailure>{
            if(!root)return root_finish_failure;
            ck::files::detail::PreparedJournalConsumerFailure first;
            // Ordinary worker, after ALL writer/native consumers have drained.
#if defined(CK_FS_ARCHIVE_READBACK_FAILURE_TEST)
            detail::archive_before_final_root_read(*root);
#endif
            for(;;){
                const bool found=ck::files::detail::read_prepared_journal_consumer_failure(*root,first);
#if defined(CK_FS_ARCHIVE_READBACK_FAILURE_TEST)
                detail::archive_final_root_read_observed(found,found&&first.pending);
#endif
                if(!found)return root_finish_failure;
                if(first.pending){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
                ck::files::NativeArchiveFailure out;out.state=first.state;out.domain=first.domain;out.code=first.code;out.operation=first.operation;return out;
            }
        };
        auto cache_error=[&](UnavailableObservation detail) {
            bindings = {};
            close_noexcept();result.failure_pod=read_root();
            if(result.failure_pod){const auto& f=*result.failure_pod;
                try{result.error=UnavailableObservation{f.state,{f.domain,f.code,f.operation.data()}};}catch(...){result.error.reset();}
            }else result.error=std::move(detail);
            const bool clean_cancelled=!result.failure_pod&&result.error&&token.stop_requested()&&
                result.error->error.domain==ErrorDomain::Adapter&&result.error->error.code==static_cast<std::int64_t>(AdapterError::InterruptedRead);
            result.state=clean_cancelled?FilesVolumeArchiveState::Cancelled:FilesVolumeArchiveState::Failed;emit(std::move(result));
        };
        try {
            if (token.stop_requested()) {
                cache_error(error(AdapterError::InterruptedRead, "volume_archive.cancelled", ObservationState::Unknown));
                return;
            }
            const auto source = bindings.source ? &bindings.source : nullptr;
            const auto source_checked = revalidate_archive_source(source, token);
            if (!source_checked.value()) { cache_error(*source_checked.unavailable_detail()); return; }
            if (destination) {
                const auto route = revalidate_archive_destination(value.destination_directory, *destination);
                if (!route.value()) { cache_error(*route.unavailable_detail()); return; }
                const auto conflict = probe_archive_destination(*destination, value.destination_directory.encoding(),
                                                                value.archive_name);
                if (!conflict.value()) { cache_error(*conflict.unavailable_detail()); return; }
                if (*conflict.value() != FilesVolumeArchiveDestinationState::Ready) {
                    cache_error(error(AdapterError::PlanDecisionStale, "volume_archive.review.destination_conflict",
                                      ObservationState::Changed));
                    return;
                }
            }
            if(value.prepared_journal_root)root=value.prepared_journal_root;
            else {auto acquired=ck::files::acquire_native_journal_root(*value.journal_root);
                if(!acquired.value()){cache_error(*acquired.unavailable_detail());return;}root=std::move(acquired).take_value();owns_root=true;}
            auto journal_name=archive_journal_name(root->encoding(),value.journal_id);
            if(!journal_name){cache_error(error(AdapterError::UnrepresentableName,"volume_archive.journal_name",ObservationState::Unsupported));return;}
            if (!destination) {
                ck::files::NativeFileSystem filesystem;
                auto opened = filesystem.open_native_directory(value.destination_directory);
                if (!opened.value()) { cache_error(*opened.unavailable_detail()); return; }
                destination = ArchiveDestination(std::move(opened).take_value());
            }
            // Admit private scope before source acquisition or any archive effect.
            auto guard=ck::files::JournalScopeGuard::create_for_namespace_child(root,destination,token);
            if(!guard.value()){destination.reset();cache_error(*guard.unavailable_detail());return;}
            auto scope=std::move(guard).take_value();
            auto document=value.build_document(token);
            if(!document.value()){scope.reset();destination.reset();cache_error(*document.unavailable_detail());return;}
            auto complete=std::move(document).take_value();complete.consistency=value.consistency;
            if(!archive_source_matches(value.source,complete.source)){
                scope.reset();destination.reset();cache_error(error(AdapterError::UnqualifiedIdentity,"volume_archive.identity_revalidation",ObservationState::Changed));return;}
            auto host=value.destination_directory.host_path();
            if(!host.value()){scope.reset();destination.reset();cache_error(*host.unavailable_detail());return;}
            // The lambda destroys request/capability copies before legacy Root
            // finish; no Root lifecycle call is made by the writer consumer.
            auto invoke=[&](){
                FilesVolumeArchiveWriteRequest request{
                    .document=std::move(complete), .destination_directory=*host.value(),
                    .archive_name=value.archive_name, .transaction_id=value.transaction_id,
                    .append_journal={}, .append_journal_evidence={}, .destination_owner=destination,
                    .prepared_journal_root=root, .native_journal_name=*journal_name,
                    .native_journal_id=value.journal_id, .journal_limits=value.journal_limits};
                return write_bound_volume_archive(request, token, source);
            };
            auto written=invoke();scope.reset();bindings = {};
            if(!written.value()){cache_error(*written.unavailable_detail());return;}
            result.result=std::move(written).take_value();
            close_noexcept();result.failure_pod=read_root();
            if(result.failure_pod)result.result->publication=FilesVolumeArchivePublication::PublishedDurabilityUnknown;
            result.state=result.result->publication==FilesVolumeArchivePublication::Durable?FilesVolumeArchiveState::Complete:FilesVolumeArchiveState::Indeterminate;
            emit(std::move(result));
        }catch(...){
            // Every writer local unwound and drained BEFORE this handler. Keep
            // native first-error POD even if diagnostic allocation stays denied.
            bindings = {};
            close_noexcept();result.failure_pod=read_root();
            if(!result.failure_pod){ck::files::NativeArchiveFailure fallback;fallback.state=ObservationState::Failed;fallback.domain=ErrorDomain::Adapter;
                fallback.code=static_cast<std::int64_t>(AdapterError::InvalidNativeResult);constexpr char op[]="volume_archive.worker_fault";
                std::copy_n(op,sizeof(op),fallback.operation.begin());result.failure_pod=fallback;}
            const auto& f=*result.failure_pod;
            try{result.error=UnavailableObservation{f.state,{f.domain,f.code,f.operation.data()}};}catch(...){result.error.reset();}
            if(result.result){result.result->publication=FilesVolumeArchivePublication::PublishedDurabilityUnknown;result.state=FilesVolumeArchiveState::Indeterminate;}
            else result.state=FilesVolumeArchiveState::WorkerFault;
            emit(std::move(result));
        }
    }
    void run_review(ReviewedJob job, ArchiveBindings bindings, std::stop_token token) {
        auto fail = [&](UnavailableObservation detail, FilesVolumeArchiveState state = FilesVolumeArchiveState::Failed) {
            bindings = {};
            FilesVolumeArchiveReply failed;
            failed.request = job.ticket;
            failed.state = state;
            failed.error = std::move(detail);
            emit(std::move(failed));
        };
        try {
            const auto &review = job.approval.review();
            if (!bindings.destination || !bindings.source) {
                fail(error(AdapterError::PlanDecisionStale, "volume_archive.review.binding_missing",
                           ObservationState::Changed));
                return;
            }
            if (token.stop_requested()) {
                fail(error(AdapterError::InterruptedRead, "volume_archive.cancelled", ObservationState::Unknown),
                     FilesVolumeArchiveState::Cancelled);
                return;
            }
            const auto generated = ck::files::new_native_operation_journal_id();
            if (!generated.value()) {
                fail(*generated.unavailable_detail());
                return;
            }
            const auto journal_id = *generated.value();
            // The archive evidence format still stores a nonzero 64-bit
            // transaction number. Exclusive journal creation uses all 128 bits.
            std::uint64_t transaction_id = 0;
            for (std::size_t offset = 0; offset < journal_id.bytes.size() && transaction_id == 0;
                 offset += sizeof(transaction_id))
                for (std::size_t byte = 0; byte < sizeof(transaction_id); ++byte)
                    transaction_id |= std::to_integer<std::uint64_t>(journal_id.bytes[offset + byte]) << (byte * 8U);
            auto build_document = [approval = job.approval](std::stop_token stop_token) {
                if (stop_token.stop_requested())
                    return review_rejected<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                        AdapterError::InterruptedRead, "volume_archive.cancelled");
                auto document = approval.review()->document();
                document.losses_reviewed = document.losses.empty() || approval.continued_with_listed_losses();
                return Observation<FilesVolumeArchiveDocument>::available(std::move(document));
            };
            FilesVolumeArchiveRequest request{
                .source=review->document().source, .destination_directory=review->destination_directory(),
                .archive_name=review->archive_name(), .consistency=review->document().consistency,
                .transaction_id=transaction_id, .build_document=std::move(build_document),
                .journal_root=std::move(job.journal_root), .journal_id=journal_id,
                .journal_limits={}, .open_journal={}, .prepared_journal_root={}};
            run_one(job.ticket, std::move(request), token, std::move(bindings));
        } catch (...) {
            bindings = {};
            FilesVolumeArchiveReply failed;
            failed.request = job.ticket;
            failed.state = FilesVolumeArchiveState::WorkerFault;
            emit(std::move(failed));
        }
    }
    void run() {
        ArchiveBindings bindings;
        for (;;) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending || discard_review; });
                if (stopping) {
                    pending.reset();
                    ready_review.reset();
                    plan_reply.reset();
                    busy = false;
                    discard_review = false;
                    return;
                }
                if (discard_review) {
                    lock.unlock();
                    bindings = {};
                    lock.lock();
                    discard_review = false;
                    continue;
                }
                job = std::move(pending);
                pending.reset();
                busy = true;
            }
            const auto token = stop.get_token();
            if (auto *plan = std::get_if<PlanJob>(&*job))
                bindings = run_plan(std::move(*plan), token);
            else if (auto *review = std::get_if<ReviewedJob>(&*job))
                run_review(std::move(*review), std::move(bindings), token);
            else {
                auto &write = std::get<WriteJob>(*job);
                run_one(std::move(write.first), std::move(write.second), token);
            }
        }
    }
};

FilesVolumeArchiveService::FilesVolumeArchiveService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesVolumeArchiveService::~FilesVolumeArchiveService() = default;
FilesVolumeArchivePlanningSubmitted FilesVolumeArchiveService::plan(FilesVolumeArchivePlanningRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return {FilesVolumeArchivePlanningSubmission::Stopped, {}};
    if (request.source.identity.empty() || request.source.mount_point.empty() ||
        !request.destination_directory.is_valid() ||
        (!request.archive_name.empty() && !safe_archive_name(request.archive_name)) || !request.capture_source)
        return {FilesVolumeArchivePlanningSubmission::Invalid, {}};
    if (state_->reserved())
        return {FilesVolumeArchivePlanningSubmission::Busy, {}};
    auto ticket = FilesVolumeArchivePlanningTicket(state_->next++);
    std::stop_source stop;
    state_->pending.emplace(State::PlanJob{ticket, std::move(request)});
    state_->latest_plan = ticket;
    state_->latest = {};
    state_->stop = std::move(stop);
    state_->condition.notify_one();
    return {FilesVolumeArchivePlanningSubmission::Accepted, ticket};
}
bool FilesVolumeArchiveService::cancel_plan(const FilesVolumeArchivePlanningTicket &ticket) {
    std::lock_guard lock(state_->mutex);
    if (!ticket.valid() || ticket != state_->latest_plan || state_->stopping)
        return false;
    if (state_->ready_review) {
        state_->ready_review.reset();
        if (state_->plan_reply) {
            state_->plan_reply->review.reset();
            state_->plan_reply->state = FilesVolumeArchivePlanningState::Cancelled;
        }
        state_->discard_review = true;
        state_->latest_plan = {};
    } else if (state_->busy || state_->pending) {
        state_->stop.request_stop();
    } else {
        return false;
    }
    state_->condition.notify_one();
    return true;
}
std::optional<FilesVolumeArchivePlanningReply> FilesVolumeArchiveService::take_plan() {
    std::lock_guard lock(state_->mutex);
    auto result = std::exchange(state_->plan_reply, std::nullopt);
    state_->signalled = state_->reply.has_value();
    return result;
}
FilesVolumeArchiveSubmitted FilesVolumeArchiveService::execute_review(
    const FilesVolumeArchivePlanningTicket &plan, FilesVolumeArchiveApproval approval,
    ck::files::NativeAbsolutePath journal_root) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return {FilesVolumeArchiveSubmission::Stopped, {}};
    if (!plan.valid() || plan != state_->latest_plan || !state_->ready_review ||
        state_->ready_review != approval.review() || !journal_root.is_valid())
        return {FilesVolumeArchiveSubmission::Invalid, {}};
    auto ticket = FilesVolumeArchiveTicket(state_->next++);
    std::stop_source stop;
    state_->pending.emplace(State::ReviewedJob{ticket, std::move(approval), std::move(journal_root)});
    state_->latest = ticket;
    state_->latest_plan = {};
    state_->ready_review.reset();
    state_->stop = std::move(stop);
    state_->condition.notify_one();
    return {FilesVolumeArchiveSubmission::Accepted, ticket};
}
FilesVolumeArchiveSubmitted FilesVolumeArchiveService::request(FilesVolumeArchiveRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return {FilesVolumeArchiveSubmission::Stopped, {}};
    if (!State::valid(request))
        return {FilesVolumeArchiveSubmission::Invalid, {}};
    if (state_->reserved())
        return {FilesVolumeArchiveSubmission::Busy, {}};
    auto ticket = FilesVolumeArchiveTicket(state_->next++);
    std::stop_source stop;
    state_->pending.emplace(State::WriteJob{ticket, std::move(request)});
    state_->latest = ticket;
    state_->latest_plan = {};
    state_->stop = std::move(stop);
    state_->condition.notify_one();
    return {FilesVolumeArchiveSubmission::Accepted, ticket};
}
bool FilesVolumeArchiveService::cancel(const FilesVolumeArchiveTicket &request) {
    std::lock_guard lock(state_->mutex);
    if (!request.valid() || request != state_->latest || (!state_->busy && !state_->pending))
        return false;
    state_->stop.request_stop();
    return true;
}
std::optional<FilesVolumeArchiveReply> FilesVolumeArchiveService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::exchange(state_->reply, std::nullopt);
    state_->signalled = state_->plan_reply.has_value();
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
    return state_->reserved();
}

} // namespace ck::vision
