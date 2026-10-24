// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_store.hpp"
namespace ck::files {
struct IndexRowKey {
    AdapterScope scope;
    std::uint64_t number;
};
enum class DirectoryIndexLookupState { Building, Ready, Failed };
class DirectoryIndexLookupJob;
// Worker-only, same-lifetime bidirectional disk lookup over one sealed store.
// Fixed-width rank/ID entries carry schema, generation, role and CRC. No native
// authority, persistent/restart format, RAM row-address table or UI-thread I/O.
// Source rows must form a permutation of original IDs 1..N. The builder verifies
// that property using initialized on-disk ID slots, without a directory-size set.
class DirectoryIndexLookup final {
  public:
    ~DirectoryIndexLookup();
    DirectoryIndexLookup(const DirectoryIndexLookup &) = delete;
    DirectoryIndexLookup &operator=(const DirectoryIndexLookup &) = delete;
    std::uint64_t rows() const noexcept {
        return rows_;
    }
    std::uint64_t bytes() const noexcept {
        return bytes_;
    }
    const AdapterScope &row_scope() const noexcept {
        return source_->row_scope();
    }
    DirectoryIndexLookupState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    // Zero-based position. Foreign/moved row keys and invalid bounds do no I/O
    // and leave a healthy lookup usable. Same row_scope survives re-sorting.
    Observation<std::uint64_t> find_row(const IndexRowKey &key, std::stop_token stop = {});
    // At most one bounded sequential source page plus two 64-byte lookup
    // records. An inadequate first-row wire budget can be retried. Source reads
    // finish one bounded page before observing cancellation of this projection.
    Observation<DirectoryIndexScanPage> read_page(std::uint64_t first_rank, DirectoryIndexScanLimits limits = {},
                                                  std::stop_token stop = {});

  private:
    friend class DirectoryIndexLookupJob;
    struct Entry {
        std::uint64_t rank, row, offset;
        std::uint32_t bytes;
    };
    DirectoryIndexLookup(std::unique_ptr<IndexByteFile> file, std::shared_ptr<DirectoryIndexStore> source,
                         std::shared_ptr<IndexStorageBudget> budget, std::uint64_t bytes);
    void fail(UnavailableObservation detail);
    void exception() noexcept;
    bool healthy();
    Observation<bool> size_matches(std::stop_token stop);
    Observation<bool> read_exact(std::uint64_t offset, std::span<std::byte> bytes, std::stop_token stop);
    Observation<bool> write_exact(std::uint64_t offset, std::span<const std::byte> bytes, std::stop_token stop);
    std::array<std::byte, 64> encode(const Entry &entry, std::uint32_t role) const;
    Observation<Entry> decode(std::span<const std::byte, 64> bytes, std::uint32_t role) const;
    Observation<Entry> entry_at(std::uint64_t index, std::uint32_t role, std::stop_token stop);
    std::unique_ptr<IndexByteFile> file_;
    std::shared_ptr<DirectoryIndexStore> source_;
    std::shared_ptr<IndexStorageBudget> budget_;
    const std::uint64_t rows_, bytes_;
    DirectoryIndexLookupState state_ = DirectoryIndexLookupState::Building;
    std::optional<UnavailableObservation> failure_;
};
enum class DirectoryIndexLookupJobState { Initializing, Building, Complete, Failed, Cancelled, Abandoned };
// Each advance writes <=64KiB initialization bytes or checks/appends one bounded
// source page. The complete table is charged before native creation/writes.
// Empty input verifies source End and needs no additional scratch file/slot.
class DirectoryIndexLookupJob final {
  public:
    using Result = std::shared_ptr<DirectoryIndexLookup>;
    DirectoryIndexLookupJob(std::shared_ptr<DirectoryIndexStore> source, DirectoryIndexScanLimits limits = {},
                            std::uint64_t max_table_bytes = 256ULL * 1024 * 1024, std::stop_token stop = {});
    DirectoryIndexLookupJob(const DirectoryIndexLookupJob &) = delete;
    DirectoryIndexLookupJob &operator=(const DirectoryIndexLookupJob &) = delete;
    DirectoryIndexLookupJobState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    DirectoryIndexLookupJobState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<Result> completed() const;

  private:
    void release() noexcept;
    void fail(UnavailableObservation detail, bool stopped = false);
    void cancel() noexcept;
    Result index_, result_;
    DirectoryIndexScanLimits limits_;
    std::optional<IndexScanCursor> cursor_;
    std::uint64_t initialized_ = 0, processed_ = 0;
    DirectoryIndexLookupJobState state_ = DirectoryIndexLookupJobState::Initializing;
    std::optional<UnavailableObservation> failure_;
};
} // namespace ck::files
