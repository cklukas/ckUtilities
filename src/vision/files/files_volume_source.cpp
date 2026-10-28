// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_source.hpp"

#include "native_filesystem.hpp"

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

std::string relative_path(const std::string &parent, const ck::files::NativeName &name) {
    const auto *bytes = name.posix_bytes();
    if (!bytes)
        return {};
    if (parent.empty())
        return *bytes;
    return parent + "/" + *bytes;
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
    const bool hard = detail->state == ObservationState::Unknown || detail->state == ObservationState::Denied ||
                      detail->state == ObservationState::Gone || detail->state == ObservationState::Changed;
    add_loss(document, entry_index, path, kind, hard,
             std::string(feature) + (hard ? " coverage is not qualified" : " is not preserved"));
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
};

bool revalidate_volume(const FilesVolumeRecord &source, UnavailableObservation &failure_detail) {
    const auto volumes = enumerate_native_volumes();
    if (!volumes.value()) {
        failure_detail = *volumes.unavailable_detail();
        return false;
    }
    const auto found = std::any_of(volumes.value()->begin(), volumes.value()->end(), [&](const auto &current) {
        return current.mount_point == source.mount_point && current.identity == source.identity;
    });
    if (found)
        return true;
    failure_detail = failure(AdapterError::MetadataChanged, "volume_archive.source.revalidation",
                             ObservationState::Changed);
    return false;
}

} // namespace

Observation<FilesVolumeArchiveDocument> build_native_volume_archive_document(
    const FilesVolumeRecord &source, FilesVolumeArchiveConsistency consistency, std::stop_token stop_token,
    FilesVolumeArchiveSourceLimits limits) {
#if defined(_WIN32)
    (void)source;
    (void)consistency;
    (void)stop_token;
    (void)limits;
    return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unsupported,
        AdapterError::UnrepresentableName, "volume_archive.source.windows_utf16_codec");
#else
    if (source.identity.empty() || source.mount_point.empty())
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
            AdapterError::UnqualifiedIdentity, "volume_archive.source.identity");
    if (source.archive.support != FilesVolumeArchiveSupport::LiveReadable &&
        source.archive.support != FilesVolumeArchiveSupport::SnapshotPreferred)
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
            AdapterError::ProviderReviewRequired, "volume_archive.source.capability");
    if (consistency == FilesVolumeArchiveConsistency::Snapshot)
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unsupported,
            AdapterError::ProviderReviewRequired, "volume_archive.source.snapshot_provider");
    if (limits.max_entries == 0 || limits.max_entries > 8'000'000 || limits.max_depth == 0 ||
        limits.max_depth > 4096 || !limits.object.query.valid() || !limits.object.retained.valid())
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Failed,
            AdapterError::InvalidLimit, "volume_archive.source.limits");
    if (stop_token.stop_requested())
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
            AdapterError::InterruptedRead, "volume_archive.source.cancelled");

    UnavailableObservation revalidation;
    if (!revalidate_volume(source, revalidation))
        return unavailable<FilesVolumeArchiveDocument>(revalidation);

    auto root_address = NativeAbsolutePath::from_posix(source.mount_point);
    if (!root_address.value())
        return unavailable<FilesVolumeArchiveDocument>(*root_address.unavailable_detail());
    NativeFileSystem filesystem;
    auto opened = filesystem.open_native_directory(*root_address.value());
    if (!opened.value())
        return unavailable<FilesVolumeArchiveDocument>(*opened.unavailable_detail());
    auto root = std::shared_ptr<DirectoryCursor>(std::move(opened).take_value());
    if (!root)
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Failed,
            AdapterError::InvalidNativeResult, "volume_archive.source.root");
    const auto root_metadata = root->inspect_basic();
    if (!root_metadata.value())
        return unavailable<FilesVolumeArchiveDocument>(*root_metadata.unavailable_detail());
    if (!root_metadata.value()->kind.value() || *root_metadata.value()->kind.value() != ObjectKind::Directory)
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unsupported,
            AdapterError::NotDirectory, "volume_archive.source.root_kind");
    if (!root_metadata.value()->identity.value() || !root_metadata.value()->mount_id.value())
        return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
            AdapterError::UnqualifiedIdentity, "volume_archive.source.root_identity");
    const auto root_mount = *root_metadata.value()->mount_id.value();

    FilesVolumeArchiveDocument document;
    document.source = source;
    document.consistency = consistency;
    document.path_encoding = ck::files::NameEncoding::PosixBytes;
    document.entries.push_back({".", FilesVolumeArchiveEntryKind::Directory, 0, {}, {}, 0});
    add_loss(document, 0, ".", FilesVolumeArchiveLossKind::Metadata, false,
             "portable root-directory metadata is not encoded");

    std::vector<Frame> frames;
    frames.push_back({std::move(root), {}, 0});
    std::vector<std::pair<ObjectIdentity, std::uint64_t>> hard_links;
    std::uint64_t next_hard_link_group = 1;

    while (!frames.empty()) {
        if (stop_token.stop_requested())
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                AdapterError::InterruptedRead, "volume_archive.source.cancelled");
        auto &frame = frames.back();
        auto batch = frame.directory->read(1, stop_token);
        if (batch.error)
            return unavailable<FilesVolumeArchiveDocument>(*batch.error);
        if (batch.state == ck::files::DirectoryReadState::Cancelled)
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                AdapterError::InterruptedRead, "volume_archive.source.cancelled");
        if (batch.names.empty()) {
            if (batch.state == ck::files::DirectoryReadState::End)
                frames.pop_back();
            continue;
        }
        if (batch.names.size() != 1 || !batch.names.front().is_valid())
            return unavailable<FilesVolumeArchiveDocument>(ObservationState::Failed,
                AdapterError::InvalidNativeResult, "volume_archive.source.name");
        const auto name = std::move(batch.names.front());
        const auto path = relative_path(frame.path, name);
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
        const auto &metadata = metadata_source->metadata();
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
            if (!same_identity(held->metadata().identity, (*upgraded.value())->metadata().identity, continuity))
                return unavailable<FilesVolumeArchiveDocument>(continuity);
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
                    if (metadata.logical_size.value() && *metadata.logical_size.value() != entry.logical_size)
                        add_loss(document, index, path, FilesVolumeArchiveLossKind::Consistency, true,
                                 "logical size changed during layout inspection");
                    const auto &held_source = (*inspected.value())->held_source_for_worker();
                    if (!held_source)
                        add_loss(document, index, path, FilesVolumeArchiveLossKind::Consistency, true,
                                 "held source was not retained for content reads");
                    else {
                        for (const auto &range : layout.value()->ranges) {
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
                                entry.extents.push_back({offset, std::move(bytes).take_value().bytes});
                                const auto count = entry.extents.back().bytes.size();
                                offset += count;
                                remaining -= count;
                                if (stop_token.stop_requested())
                                    return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                                        AdapterError::InterruptedRead, "volume_archive.source.cancelled");
                            }
                        }
                    }
                }
            }
        } else if (kind == ObjectKind::SymbolicLink) {
            entry.kind = FilesVolumeArchiveEntryKind::Symlink;
            const auto &link = data.link;
            if (link.value()) {
                if (const auto *posix = std::get_if<ck::files::PosixSymlinkData>(&link.value()->payload))
                    entry.link_target = posix->target;
                else
                    add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                             "link representation is not a POSIX symlink");
            } else {
                add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                         "symlink target could not be read");
            }
        } else {
            entry.kind = FilesVolumeArchiveEntryKind::Unsupported;
            add_loss(document, index, path, FilesVolumeArchiveLossKind::UnsupportedObject, true,
                     "object kind is not representable in a filesystem archive");
        }
        document.entries.push_back(std::move(entry));

        if (kind == ObjectKind::Directory) {
            if (frame.depth >= limits.max_depth)
                return unavailable<FilesVolumeArchiveDocument>(ObservationState::Unknown,
                    AdapterError::InventoryLimit, "volume_archive.source.depth");
            auto child = frame.directory->open_directory_child(name);
            if (!child.value())
                return unavailable<FilesVolumeArchiveDocument>(*child.unavailable_detail());
            auto child_cursor = std::shared_ptr<DirectoryCursor>(std::move(child).take_value());
            if (!child_cursor || !child_cursor->identity().value() ||
                !same_identity(*metadata.identity.value(), *child_cursor->identity().value()))
                return unavailable<FilesVolumeArchiveDocument>(ObservationState::Changed,
                    AdapterError::MetadataChanged, "volume_archive.source.directory_identity");
            frames.push_back({std::move(child_cursor), path, frame.depth + 1});
        }
    }

    if (!revalidate_volume(source, revalidation))
        return unavailable<FilesVolumeArchiveDocument>(revalidation);
    return Observation<FilesVolumeArchiveDocument>::available(std::move(document));
#endif
}

} // namespace ck::vision
