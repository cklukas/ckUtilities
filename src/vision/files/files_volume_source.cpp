// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_source.hpp"

#include "native_filesystem.hpp"
#include "native_index_file.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ck::vision {
namespace {
using ck::files::AdapterError;
using ck::files::BasicMetadata;
using ck::files::DirectoryCursor;
using ck::files::EntryAccess;
using ck::files::ErrorDomain;
using ck::files::HeldChild;
using ck::files::NativeAbsolutePath;
using ck::files::NativeFileSystem;
using ck::files::ObjectIdentity;
using ck::files::ObjectInventoryJob;
using ck::files::ObjectInventoryJobState;
using ck::files::ObjectKind;
using ck::files::Observation;
using ck::files::ObservationState;
using ck::files::UnavailableObservation;

UnavailableObservation failure(AdapterError code, const char *operation,
                               ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

template <class T>
Observation<T> unavailable(const UnavailableObservation &detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}

template <class T>
Observation<T> unavailable(AdapterError code, const char *operation,
                           ObservationState state = ObservationState::Failed) {
    return unavailable<T>(failure(code, operation, state));
}

template <class T>
Observation<T> unavailable(ObservationState state, AdapterError code, const char *operation) {
    return unavailable<T>(failure(code, operation, state));
}

bool same_identity(const Observation<ObjectIdentity> &left, const Observation<ObjectIdentity> &right,
                  UnavailableObservation &failure_detail) {
    if (!left.value()) {
        failure_detail = *left.unavailable_detail();
        return false;
    }
    if (!right.value()) {
        failure_detail = *right.unavailable_detail();
        return false;
    }
    const auto compared = ck::files::compare_identity(*left.value(), *right.value());
    if (compared == ck::files::IdentityComparison::Same)
        return true;
    failure_detail = failure(compared == ck::files::IdentityComparison::Different
                                  ? AdapterError::MetadataChanged
                                  : AdapterError::UnqualifiedIdentity,
                              "volume_archive.source.identity",
                              compared == ck::files::IdentityComparison::Different
                                  ? ObservationState::Changed
                                  : ObservationState::Unknown);
    return false;
}

bool same_identity(const ObjectIdentity &left, const ObjectIdentity &right) {
    return ck::files::compare_identity(left, right) == ck::files::IdentityComparison::Same;
}

std::string utf16_bytes(std::u16string_view units) {
    std::string result;
    result.reserve(units.size() * 2);
    for (const auto unit : units) {
        result.push_back(static_cast<char>(unit & 0xff));
        result.push_back(static_cast<char>(unit >> 8));
    }
    return result;
}

std::string archive_root_path(ck::files::NameEncoding encoding) {
    return encoding == ck::files::NameEncoding::WindowsUtf16 ? utf16_bytes(u".") : ".";
}

std::string relative_path(const std::string &parent, const ck::files::NativeName &name,
                         ck::files::NameEncoding encoding) {
    std::string component;
    if (encoding == ck::files::NameEncoding::PosixBytes) {
        const auto *bytes = name.posix_bytes();
        if (!bytes)
            return {};
        component = *bytes;
    } else {
        const auto *units = name.windows_units();
        if (!units)
            return {};
        component = utf16_bytes(*units);
    }
    if (parent.empty() || parent == archive_root_path(encoding))
        return component;
    const std::string separator = encoding == ck::files::NameEncoding::WindowsUtf16
                                       ? utf16_bytes(u"\\")
                                       : "/";
    return parent + separator + component;
}

void add_loss(FilesVolumeArchiveDocument &document, std::size_t entry_index, std::string path,
              FilesVolumeArchiveLossKind kind, bool hard, std::string diagnostic) {
    document.losses.push_back(
        {entry_index, std::move(path), kind, hard, std::move(diagnostic)});
}

void add_observation_loss(FilesVolumeArchiveDocument &document, std::size_t entry_index,
                          const std::string &path, FilesVolumeArchiveLossKind kind,
                          const char *feature, const auto &observation) {
    if (observation.value())
        return;
    const auto *detail = observation.unavailable_detail();
    if (!detail)
        return;
    const bool hard = detail->state != ObservationState::Unsupported;
    std::string diagnostic = std::string(feature) + (hard ? " coverage could not be verified" : " is not preserved");
    if (hard && !detail->error.operation.empty())
        diagnostic += " (" + detail->error.operation + ')';
    add_loss(document, entry_index, path, kind, hard, std::move(diagnostic));
}

void add_metadata_losses(FilesVolumeArchiveDocument &document, std::size_t entry_index,
                         const std::string &path, const ck::files::ObjectInventoryData &data) {
    // The current archive manifest carries data/layout and links only. The
    // review must therefore show the basic metadata that will not be replayed,
    // even when it was observed successfully.
    add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Metadata, false,
             "portable timestamps, ownership, permissions and flags are not encoded");

    if (data.channels.value()) {
        const auto &catalog = *data.channels.value();
        if (catalog.coverage == ck::files::DataChannelCoverage::Unknown)
            add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::AdditionalChannel, true,
                     "additional data-channel coverage is unknown");
        else if (!catalog.channels.empty())
            add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::AdditionalChannel, false,
                     "additional data channels are not encoded");
    } else {
        add_observation_loss(document, entry_index, path, FilesVolumeArchiveLossKind::AdditionalChannel,
                             "additional data-channel", data.channels);
    }

    if (data.attributes.value()) {
        const auto &catalog = *data.attributes.value();
        if (catalog.coverage == ck::files::AttributeCoverage::Unknown)
            add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Metadata, true,
                     "extended-attribute coverage is unknown");
        else if (!catalog.attributes.empty())
            add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Metadata, false,
                     "extended attributes are not encoded");
    } else {
        add_observation_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Metadata,
                             "extended-attribute", data.attributes);
    }
    for (const auto &attribute : data.attribute_values) {
        const auto *failure = attribute.value.unavailable_detail();
        // The catalog already reports the loss of known attributes. Failed
        // reads require a separate, non-approvable coverage diagnostic.
        if (failure && failure->state != ObservationState::Unsupported)
            add_observation_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Metadata,
                                 "extended-attribute value", attribute.value);
    }

    for (const auto &security : data.security) {
        if (security.value()) {
            add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Security, false,
                     "security descriptor or ACL is not encoded");
        } else {
            add_observation_loss(document, entry_index, path, FilesVolumeArchiveLossKind::Security,
                                 "security metadata", security);
        }
    }
    if (data.integrity.value() || data.inode_flags.value())
        add_loss(document, entry_index, path, FilesVolumeArchiveLossKind::StorageRepresentation, false,
                 "provider storage state is not encoded");
}

Observation<std::shared_ptr<const ck::files::CompletedObjectInventory>> inventory(
    const HeldChild::Pointer &held, const FilesVolumeArchiveSourceLimits &limits, std::stop_token stop) {
    auto job = ObjectInventoryJob::for_held_source(held, limits.object);
    for (;;) {
        if (stop.stop_requested())
            return unavailable<std::shared_ptr<const ck::files::CompletedObjectInventory>>(
                AdapterError::InterruptedRead, "volume_archive.source.cancelled", ObservationState::Unknown);
        const auto state = job->advance(stop);
        if (state == ObjectInventoryJobState::Paused)
            continue;
        if (state == ObjectInventoryJobState::Failed)
            return unavailable<std::shared_ptr<const ck::files::CompletedObjectInventory>>(*job->failure());
        if (state == ObjectInventoryJobState::Complete) {
            auto completed = job->completed();
            if (completed.value())
                return Observation<std::shared_ptr<const ck::files::CompletedObjectInventory>>::available(
                    *completed.value());
            return unavailable<std::shared_ptr<const ck::files::CompletedObjectInventory>>(
                completed.unavailable_detail() ? *completed.unavailable_detail()
                                               : failure(AdapterError::PlanInvalid, "volume_archive.source.inventory"));
        }
    }
}

struct Frame final {
    std::shared_ptr<DirectoryCursor> directory;
    std::string path;
    std::size_t depth = 0;
    BasicMetadata before;
};

// These are live bookends for detecting observed changes while capturing an
// entry. Reading can update atime. Equal observations do not freeze the object.
Observation<bool> check_capture_version(const BasicMetadata &before,
                                        const Observation<BasicMetadata> &observed,
                                        const char *changed_operation) {
    if (!observed.value())
        return unavailable<bool>(*observed.unavailable_detail());
    const auto &after = *observed.value();
    UnavailableObservation identity_failure;
    if (!same_identity(before.identity, after.identity, identity_failure))
        return unavailable<bool>(identity_failure);
    if (!before.kind.value() || !after.kind.value() || !before.mount_id.value() || !after.mount_id.value() ||
        !before.link_count.value() || !after.link_count.value() || !before.modified.value() ||
        !after.modified.value() || !before.changed.value() || !after.changed.value() ||
        (*before.kind.value() == ObjectKind::RegularFile &&
         (!before.logical_size.value() || !after.logical_size.value())))
        return unavailable<bool>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity,
                                 "volume_archive.source.version_unavailable");
    if (*before.kind.value() != *after.kind.value() || *before.mount_id.value() != *after.mount_id.value() ||
        *before.link_count.value() != *after.link_count.value() || *before.modified.value() != *after.modified.value() ||
        *before.changed.value() != *after.changed.value() ||
        (*before.kind.value() == ObjectKind::RegularFile &&
         *before.logical_size.value() != *after.logical_size.value()))
        return unavailable<bool>(ObservationState::Changed, AdapterError::MetadataChanged, changed_operation);
    return Observation<bool>::available(true);
}

bool same_mount_root(const FilesVolumeMountRoot &expected, const BasicMetadata &actual) {
    return actual.kind.value() && *actual.kind.value() == ObjectKind::Directory &&
           actual.identity.value() && actual.mount_id.value() &&
           *actual.mount_id.value() == expected.mount_id &&
           same_identity(expected.identity, *actual.identity.value());
}

Observation<bool> revalidate_source_root(const NativeAbsolutePath &route, const DirectoryCursor &held,
                                        const FilesVolumeMountRoot &expected, std::stop_token stop_token) {
    if (stop_token.stop_requested())
        return unavailable<bool>(ObservationState::Unknown, AdapterError::InterruptedRead,
                                  "volume_archive.source.cancelled");
    const auto original = held.inspect_basic();
    if (!original.value())
        return unavailable<bool>(*original.unavailable_detail());
    // A second native open checks the route. All source bytes were read through
    // the retained owner, so this observation cannot redirect the capture.
    NativeFileSystem filesystem(expected.identity.scope);
    const auto current = filesystem.open_native_directory(route);
    if (!current.value()) {
        const auto &detail = *current.unavailable_detail();
        if (detail.state != ObservationState::Gone &&
            (detail.error.domain != ErrorDomain::Adapter ||
             detail.error.code != static_cast<std::int64_t>(AdapterError::FinalLinkRejected)))
            return unavailable<bool>(detail);
    } else {
        const auto metadata = (*current.value())->inspect_basic();
        if (!metadata.value())
            return unavailable<bool>(*metadata.unavailable_detail());
        if (same_mount_root(expected, *original.value()) && same_mount_root(expected, *metadata.value()))
            return Observation<bool>::available(true);
    }
    return unavailable<bool>(ObservationState::Changed, AdapterError::MetadataChanged,
                              "volume_archive.source.mount_changed");
}

struct ArchiveScratch final {
    std::unique_ptr<ck::files::IndexByteFile> file;
    std::uint64_t limit = 0;
    std::uint64_t written = 0;
    ck::files::Sha256 digest;

    Observation<bool> append(std::span<const std::byte> bytes, std::stop_token stop_token) {
        if (bytes.size() > limit - written)
            return unavailable<bool>(ObservationState::Unknown, AdapterError::InventoryLimit,
                                     "volume_archive.source.scratch_limit");
        while (!bytes.empty()) {
            if (stop_token.stop_requested())
                return unavailable<bool>(ObservationState::Unknown, AdapterError::InterruptedRead,
                                         "volume_archive.source.cancelled");
            const auto count = file->write_at(written, bytes);
            if (!count.value())
                return unavailable<bool>(*count.unavailable_detail());
            if (*count.value() == 0 || *count.value() > bytes.size())
                return unavailable<bool>(AdapterError::InvalidNativeResult, "volume_archive.source.scratch_write");
            digest.update(bytes.first(*count.value()));
            written += *count.value();
            bytes = bytes.subspan(*count.value());
        }
        return Observation<bool>::available(true);
    }
};

Observation<FilesVolumeArchiveDocument> capture_volume_document(
    const FilesVolumeRecord &source, FilesVolumeArchiveConsistency consistency, std::stop_token stop_token,
    const FilesVolumeArchiveSourceLimits &limits, std::shared_ptr<DirectoryCursor> root,
    ck::files::NameEncoding path_encoding, std::uint64_t root_mount, BasicMetadata root_metadata,
    ArchiveScratch &scratch) {
    FilesVolumeArchiveDocument document;
    document.source = source;
    document.consistency = consistency;
    document.path_encoding = path_encoding;
    const auto root_path = archive_root_path(path_encoding);
    document.entries.push_back({root_path, FilesVolumeArchiveEntryKind::Directory, 0, {}, {}, 0});
    add_loss(document, 0, root_path, FilesVolumeArchiveLossKind::Metadata, false,
             "portable root-directory metadata is not encoded");

    std::vector<Frame> frames;
    frames.push_back({std::move(root), {}, 0, std::move(root_metadata)});
    std::vector<std::pair<ObjectIdentity, std::uint64_t>> hard_links;
    std::uint64_t next_hard_link_group = 1;
    constexpr auto max_logical_size = ck::files::NativeArchiveWriteRequest::default_max_logical_size;
    constexpr auto max_data_extents = ck::files::NativeArchiveWriteRequest::default_max_extents - 2;
    std::uint64_t logical_bytes = 0;
    std::size_t extent_count = 0;
    FilesVolumeArchiveManifestBudget manifest_budget;
    const auto manifest_limit = [] {
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown, AdapterError::InventoryLimit,
                                                        "volume_archive.manifest_limit");
    };
    if (!manifest_budget.add(document.source))
        return manifest_limit();
    std::size_t accounted_entries = 0;
    std::size_t accounted_losses = 0;

    while (!frames.empty()) {
        if (stop_token.stop_requested())
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                AdapterError::InterruptedRead, "volume_archive.source.cancelled");
        while (accounted_entries < document.entries.size())
            if (!manifest_budget.add(document.entries[accounted_entries++]))
                return manifest_limit();
        while (accounted_losses < document.losses.size())
            if (!manifest_budget.add(document.losses[accounted_losses++]))
                return manifest_limit();
        if (!manifest_budget.fits_image(logical_bytes))
            return manifest_limit();
        auto &frame = frames.back();
        auto batch = frame.directory->read(1, stop_token);
        if (batch.error)
            return unavailable<FilesVolumeArchiveDocument>(*batch.error);
        if (batch.state == ck::files::DirectoryReadState::Cancelled)
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                AdapterError::InterruptedRead, "volume_archive.source.cancelled");
        if (batch.names.empty()) {
            if (batch.state == ck::files::DirectoryReadState::End) {
                const auto consistent = check_capture_version(frame.before, frame.directory->inspect_basic(),
                                                              "volume_archive.source.directory_changed");
                if (!consistent.value())
                    return unavailable<FilesVolumeArchiveDocument>(*consistent.unavailable_detail());
                frames.pop_back();
            }
            continue;
        }
        if (batch.names.size() != 1 || !batch.names.front().is_valid())
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Failed,
                AdapterError::InvalidNativeResult, "volume_archive.source.name");
        const auto name = std::move(batch.names.front());
        const auto path = relative_path(frame.path, name, path_encoding);
        if (path.empty())
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unsupported,
                AdapterError::UnrepresentableName, "volume_archive.source.name_encoding");
        if (document.entries.size() >= limits.max_entries)
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                AdapterError::InventoryLimit, "volume_archive.source.entries");

        auto metadata_only = HeldChild::observe_with_access(frame.directory, name, EntryAccess::MetadataOnly, stop_token);
        if (!metadata_only.value())
            return unavailable<FilesVolumeArchiveDocument>(*metadata_only.unavailable_detail());
        auto metadata_source = std::move(metadata_only).take_value();
        // The content-read upgrade may release the original metadata owner.
        const auto metadata = metadata_source->metadata();
        if (!metadata.kind.value() || !metadata.identity.value())
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                AdapterError::UnqualifiedIdentity, "volume_archive.source.child_identity");
        if (!metadata.mount_id.value()) {
            const auto index = document.entries.size();
            document.entries.push_back({path, FilesVolumeArchiveEntryKind::Unsupported, 0, {}, {}, 0});
            add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                     "child mount identity is unavailable; mount confinement cannot be proven");
            continue;
        }
        const auto index = document.entries.size();
        const auto kind = *metadata.kind.value();
        if (*metadata.mount_id.value() != root_mount) {
            document.entries.push_back({path, kind == ObjectKind::Directory ? FilesVolumeArchiveEntryKind::Directory
                                                                               : FilesVolumeArchiveEntryKind::Unsupported,
                                        0, {}, {}, 0});
            add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                     "mount boundary was not traversed");
            continue;
        }

        bool hard_link_alias = false;
        std::uint64_t hard_link_group = 0;
        if (kind == ObjectKind::RegularFile && metadata.link_count.value() && *metadata.link_count.value() > 1) {
            for (const auto &[identity, group] : hard_links) {
                if (same_identity(identity, *metadata.identity.value())) {
                    hard_link_alias = true;
                    hard_link_group = group;
                    break;
                }
            }
            if (!hard_link_alias) {
                hard_link_group = next_hard_link_group++;
                hard_links.emplace_back(*metadata.identity.value(), hard_link_group);
            }
        }

        HeldChild::Pointer held = std::move(metadata_source);
        if (kind == ObjectKind::RegularFile && !hard_link_alias) {
            const auto access = EntryAccess::MetadataAndLayout | EntryAccess::ContentRead;
            auto upgraded = HeldChild::observe_with_access(frame.directory, name, access, stop_token);
            if (!upgraded.value())
                return unavailable<FilesVolumeArchiveDocument>(*upgraded.unavailable_detail());
            UnavailableObservation continuity;
            const auto &upgraded_metadata = (*upgraded.value())->metadata();
            if (!same_identity(metadata.identity, upgraded_metadata.identity, continuity))
                return unavailable<FilesVolumeArchiveDocument>(continuity);
            if (!upgraded_metadata.mount_id.value() || *upgraded_metadata.mount_id.value() != root_mount)
                return unavailable<FilesVolumeArchiveDocument>(ObservationState::Changed,
                    AdapterError::MetadataChanged, "volume_archive.source.child_mount_changed");
            held = std::move(upgraded).take_value();
        }

        auto inspected = inventory(held, limits, stop_token);
        if (!inspected.value())
            return unavailable<FilesVolumeArchiveDocument>(*inspected.unavailable_detail());
        const auto &data = (*inspected.value())->data();
        add_metadata_losses(document, index, path, data);

        FilesVolumeArchiveEntry entry;
        entry.path = path;
        entry.hard_link_group = hard_link_group;
        if (kind == ObjectKind::Directory) {
            entry.kind = FilesVolumeArchiveEntryKind::Directory;
        } else if (kind == ObjectKind::RegularFile) {
            if (hard_link_alias) {
                entry.kind = FilesVolumeArchiveEntryKind::HardLink;
            } else {
                entry.kind = FilesVolumeArchiveEntryKind::RegularFile;
                const auto &layout = data.layout;
                if (!layout.value()) {
                    add_loss(document, index, path, FilesVolumeArchiveLossKind::StorageRepresentation, true,
                             "sparse data layout is unavailable; dense fallback is forbidden");
                    entry.logical_size = metadata.logical_size.value() ? *metadata.logical_size.value() : 0;
                } else {
                    entry.logical_size = layout.value()->logical_size;
                    if (entry.logical_size > max_logical_size - logical_bytes ||
                        layout.value()->ranges.size() > max_data_extents - extent_count)
                        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                            AdapterError::InventoryLimit, "volume_archive.source.image_limits");
                    logical_bytes += entry.logical_size;
                    extent_count += layout.value()->ranges.size();
                    if (metadata.logical_size.value() && *metadata.logical_size.value() != entry.logical_size)
                        add_loss(document, index, path, FilesVolumeArchiveLossKind::Consistency, true,
                                 "logical size changed during layout inspection");
                    const auto &held_source = (*inspected.value())->held_source_for_worker();
                    if (!held_source)
                        add_loss(document, index, path, FilesVolumeArchiveLossKind::Consistency, true,
                                 "held source was not retained for content reads");
                    else {
                        for (const auto &range : layout.value()->ranges) {
                            if (range.length > scratch.limit - scratch.written)
                                return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                                    AdapterError::InventoryLimit, "volume_archive.source.scratch_limit");
                            std::uint64_t offset = range.offset;
                            std::uint64_t remaining = range.length;
                            while (remaining != 0) {
                                const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
                                    remaining, ck::files::ContentReadLimits::hard_max_bytes));
                                auto bytes = held_source->read_content({chunk}, offset, stop_token);
                                if (!bytes.value() || bytes.value()->bytes.empty() ||
                                    bytes.value()->bytes.size() > chunk) {
                                    add_loss(document, index, path, FilesVolumeArchiveLossKind::Data, true,
                                             "data extent could not be read completely");
                                    break;
                                }
                                const auto &data_bytes = bytes.value()->bytes;
                                const auto stored = scratch.append(data_bytes, stop_token);
                                if (!stored.value())
                                    return unavailable<FilesVolumeArchiveDocument>(*stored.unavailable_detail());
                                const auto count = data_bytes.size();
                                offset += count;
                                remaining -= count;
                                if (stop_token.stop_requested())
                                    return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                                        AdapterError::InterruptedRead, "volume_archive.source.cancelled");
                            }
                            if (offset != range.offset)
                                entry.extents.push_back({range.offset, {}, offset - range.offset});
                        }
                    }
                }
            }
        } else if (kind == ObjectKind::SymbolicLink) {
            entry.kind = FilesVolumeArchiveEntryKind::Symlink;
            const auto &link = data.link;
            if (link.value()) {
                if (const auto *posix = std::get_if<ck::files::PosixSymlinkData>(&link.value()->payload)) {
                    entry.link_target = posix->target;
                } else if (const auto *windows = std::get_if<ck::files::WindowsReparseData>(&link.value()->payload);
                           windows && windows->kind == ck::files::WindowsReparseKind::SymbolicLink &&
                           windows->names) {
                    entry.link_target = utf16_bytes(windows->names->print);
                } else {
                    entry.kind = FilesVolumeArchiveEntryKind::Unsupported;
                    add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                             "link representation is not a POSIX symlink");
                }
            } else {
                entry.kind = FilesVolumeArchiveEntryKind::Unsupported;
                add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                         "symlink target could not be read");
            }
        } else {
            entry.kind = FilesVolumeArchiveEntryKind::Unsupported;
            add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                     "object kind is not representable in a filesystem archive");
        }
        if (kind == ObjectKind::Directory) {
            if (frame.depth >= limits.max_depth)
                return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                    AdapterError::InventoryLimit, "volume_archive.source.depth");
            auto child = frame.directory->open_directory_child(name);
            if (!child.value())
                return unavailable<FilesVolumeArchiveDocument>(*child.unavailable_detail());
            auto child_cursor = std::shared_ptr<DirectoryCursor>(std::move(child).take_value());
            const auto current = child_cursor->inspect_basic();
            if (!current.value())
                return unavailable<FilesVolumeArchiveDocument>(*current.unavailable_detail());
            if (!same_mount_root({*metadata.identity.value(), root_mount}, *current.value()))
                return unavailable<FilesVolumeArchiveDocument>(ObservationState::Changed,
                    AdapterError::MetadataChanged, "volume_archive.source.directory_identity");
            const auto consistent = check_capture_version(metadata, current, "volume_archive.source.directory_changed");
            if (!consistent.value())
                return unavailable<FilesVolumeArchiveDocument>(*consistent.unavailable_detail());
            frames.push_back({std::move(child_cursor), path, frame.depth + 1, *current.value()});
        } else if (entry.kind != FilesVolumeArchiveEntryKind::Unsupported) {
            const auto consistent = check_capture_version(metadata, held->inspect_basic(),
                                                          "volume_archive.source.entry_changed");
            if (!consistent.value())
                return unavailable<FilesVolumeArchiveDocument>(*consistent.unavailable_detail());
        }
        document.entries.push_back(std::move(entry));
    }

    return Observation<FilesVolumeArchiveDocument>::available(std::move(document));
}

} // namespace

Observation<FilesVolumeArchiveSourceCapture> capture_native_volume_archive_source(
    const FilesVolumeRecord &source, const NativeAbsolutePath &scratch_root,
    FilesVolumeArchiveConsistency consistency, std::stop_token stop_token,
    FilesVolumeArchiveSourceLimits limits) {
    if (source.identity.empty() || source.mount_point.empty())
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Unknown,
            AdapterError::UnqualifiedIdentity, "volume_archive.source.identity");
#if defined(_WIN32)
    if (source.native_mount_units.empty())
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Unsupported,
            AdapterError::UnrepresentableName, "volume_archive.source.windows_native_mount");
#endif
    if (source.archive.support != FilesVolumeArchiveSupport::LiveReadable &&
        source.archive.support != FilesVolumeArchiveSupport::SnapshotPreferred)
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Unknown,
            AdapterError::ProviderReviewRequired, "volume_archive.source.capability");
    if (consistency == FilesVolumeArchiveConsistency::Snapshot)
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Unsupported,
            AdapterError::ProviderReviewRequired, "volume_archive.source.snapshot_provider");
    if (limits.max_entries == 0 || limits.max_entries > 8'000'000 || limits.max_depth == 0 ||
        limits.max_depth > 4096 || !limits.object.query.valid() || !limits.object.retained.valid())
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Failed,
            AdapterError::InvalidLimit, "volume_archive.source.limits");
    if (stop_token.stop_requested())
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Unknown,
            AdapterError::InterruptedRead, "volume_archive.source.cancelled");

    if (!source.archive.root)
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Changed,
            AdapterError::UnqualifiedIdentity, "volume_archive.source.unbound_mount");

    const auto expected = *source.archive.root;
    auto root_address =
#if defined(_WIN32)
        NativeAbsolutePath::from_windows(source.native_mount_units);
#else
        NativeAbsolutePath::from_posix(source.mount_point);
#endif
    if (!root_address.value())
        return unavailable<FilesVolumeArchiveSourceCapture>(*root_address.unavailable_detail());
    NativeFileSystem filesystem(expected.identity.scope);
    auto opened = filesystem.open_native_directory(*root_address.value());
    if (!opened.value())
        return unavailable<FilesVolumeArchiveSourceCapture>(*opened.unavailable_detail());
    auto root = std::shared_ptr<DirectoryCursor>(std::move(opened).take_value());
    if (!root)
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Failed,
            AdapterError::InvalidNativeResult, "volume_archive.source.root");
    // Scratch creation can change directory version metadata. Establish the
    // capture baseline after the private file has left the namespace.
    auto file = ck::files::create_native_detached_index_file(scratch_root);
    if (!file.value())
        return unavailable<FilesVolumeArchiveSourceCapture>(*file.unavailable_detail());
    ArchiveScratch scratch{std::move(file).take_value(), limits.max_payload_bytes, 0, {}};
    const auto root_metadata = root->inspect_basic();
    if (!root_metadata.value())
        return unavailable<FilesVolumeArchiveSourceCapture>(*root_metadata.unavailable_detail());
    if (!same_mount_root(expected, *root_metadata.value()))
        return unavailable<FilesVolumeArchiveSourceCapture>(ObservationState::Changed,
            AdapterError::MetadataChanged, "volume_archive.source.mount_changed");

    auto document = capture_volume_document(source, consistency, stop_token, limits, root,
                                             root_address.value()->encoding(), expected.mount_id, *root_metadata.value(),
                                             scratch);
    if (!document.value())
        return unavailable<FilesVolumeArchiveSourceCapture>(*document.unavailable_detail());
    const auto route = std::move(root_address).take_value();
    const auto bound = revalidate_source_root(route, *root, expected, stop_token);
    if (!bound.value())
        return unavailable<FilesVolumeArchiveSourceCapture>(*bound.unavailable_detail());
    auto revalidate = [route, held_root = std::move(root), expected](std::stop_token token) {
        return revalidate_source_root(route, *held_root, expected, token);
    };
    FilesVolumeArchivePayload payload;
    if (scratch.written != 0)
        payload = {std::move(scratch.file), scratch.digest.digest(), scratch.limit};
    return Observation<FilesVolumeArchiveSourceCapture>::available(
        {std::move(document).take_value(), std::move(revalidate), std::move(payload)});
}

} // namespace ck::vision
