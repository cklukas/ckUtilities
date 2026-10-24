// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_record.hpp"
#include <stop_token>

namespace ck::files {
// Worker-only injection boundary, separate from the read-only source adapter.
// Implementations must exclusively own a NEW, EMPTY, private scratch file,
// reject external access, and close/remove it in their noexcept destructor.
// No default temp path, path reopening, source-file writes or durability claim.
// Counts may be short; zero progress and out-of-range counts are errors.
class IndexByteFile {
  public:
    virtual ~IndexByteFile() = default;
    virtual Observation<std::uint64_t> size() = 0;
    virtual Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) = 0;
    virtual Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) = 0;
};

// Worker-only creation boundary. A factory must clean any private creation
// work on failure/exception, and return an exclusively owned empty byte file.
// Keeping the factory alive may retain a native directory; callers must retire
// the final factory owner on its worker, never during UI dispatch.
class IndexFileFactory {
  public:
    virtual ~IndexFileFactory() = default;
    virtual Observation<std::unique_ptr<IndexByteFile>> create_file() = 0;
};

// A shared, worker-serialized logical-byte/file budget across all live indexes
// and sort runs. Includes failed/partially written files until close/removal.
// This bounds requested logical bytes, not filesystem allocation/metadata or
// other processes' disk use. Native disk-full errors remain independent.
class IndexStorageBudget final {
  public:
    IndexStorageBudget(std::uint64_t bytes, std::size_t files);
    IndexStorageBudget(const IndexStorageBudget &) = delete;
    IndexStorageBudget &operator=(const IndexStorageBudget &) = delete;
    std::uint64_t charged_bytes() const noexcept {
        return used_bytes_;
    }
    std::size_t charged_files() const noexcept {
        return used_files_;
    }
    std::uint64_t peak_charged_bytes() const noexcept {
        return peak_bytes_;
    }
    std::size_t peak_charged_files() const noexcept {
        return peak_files_;
    }

  private:
    friend class DirectoryIndexStore;
    class FileSlot;
    const std::uint64_t max_bytes_;
    const std::size_t max_files_;
    std::uint64_t used_bytes_ = 0;
    std::size_t used_files_ = 0;
    std::uint64_t peak_bytes_ = 0;
    std::size_t peak_files_ = 0;
};

struct DirectoryIndexStoreLimits {
    std::uint64_t rows = 1'000'000;
    std::uint64_t bytes = 1024ULL * 1024 * 1024;
    DirectoryIndexRecordLimits record;
};
enum class DirectoryIndexStoreState { Writing, Sealed, Failed };
// Opaque same-lifetime location. No native owner, object authority or persistence
// across restart. Addresses are issued while writing but readable only at seal.
class IndexRecordAddress final {
  public:
    std::uint64_t row_id() const noexcept {
        return row_;
    }

  private:
    friend class DirectoryIndexStore;
    IndexRecordAddress(AdapterScope owner, std::uint64_t row, std::uint64_t offset, std::size_t bytes)
        : owner_(std::move(owner)), row_(row), offset_(offset), bytes_(bytes) {
    }
    AdapterScope owner_;
    std::uint64_t row_, offset_;
    std::size_t bytes_;
};

struct DirectoryIndexScanLimits {
    std::size_t rows = 256;
    std::size_t wire_bytes = 4 * 1024 * 1024;
    static constexpr std::size_t hard_rows = 4096;
    static constexpr std::size_t hard_wire_bytes = 64 * 1024 * 1024;
};
// Same-lifetime worker cursor: only a revocation namespace and counters, never
// a file/native owner or a serialized/restart address. Copying permits retries;
// only a successfully returned page advances the caller's cursor.
class IndexScanCursor final {
  public:
    std::uint64_t position() const noexcept {
        return position_;
    }

  private:
    friend class DirectoryIndexStore;
    IndexScanCursor(AdapterScope owner, std::uint64_t position, std::uint64_t offset)
        : owner_(std::move(owner)), position_(position), offset_(offset) {
    }
    AdapterScope owner_;
    std::uint64_t position_, offset_;
};
struct DirectoryIndexScanRow {
    IndexRecordAddress address;
    DirectoryIndexRecord record;
};
struct DirectoryIndexScanPage {
    std::vector<DirectoryIndexScanRow> rows;
    IndexScanCursor next;
    std::size_t wire_bytes; // emitted records only, excludes lookahead I/O
    bool end;
};

// Worker-only bounded append/seal/read store. No per-row offset table or RAM
// fallback; the caller may stream addresses into a separate bounded sort index.
// Any I/O/corruption/cancellation failure revokes the complete store. Preflight
// input/budget rejection writes nothing and leaves prior records usable.
class DirectoryIndexStore final {
  public:
    // Reserves the shared file slot BEFORE calling the factory. Invalid input,
    // exhausted quota and an already requested stop never invoke creation.
    // The factory remains owned through file closure. No partial owner is
    // published after failure, cancellation or exception.
    static Observation<std::unique_ptr<DirectoryIndexStore>>
    create_new(std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<IndexStorageBudget> budget,
               DirectoryIndexContext context, DirectoryIndexStoreLimits limits = {}, std::stop_token stop = {});
    ~DirectoryIndexStore();
    DirectoryIndexStore(const DirectoryIndexStore &) = delete;
    DirectoryIndexStore &operator=(const DirectoryIndexStore &) = delete;
    // Stable original-row namespace, retained across trusted sorted projections.
    // It owns no source/native object and is not operation authority.
    const AdapterScope &row_scope() const noexcept {
        return row_scope_;
    }
    Observation<IndexRecordAddress> append(const DirectoryIndexRecord &record, std::stop_token stop = {});
    Observation<bool> seal(std::stop_token stop = {});
    Observation<DirectoryIndexRecord> read(const IndexRecordAddress &address, std::stop_token stop = {});
    Observation<IndexScanCursor> begin_scan() const;
    // Worker I/O; no offset table or retained prefix. Rows and aggregate encoded
    // bytes bound each returned page. An oversized first row returns a page
    // budget error without revoking the sealed store, permitting a bounded retry.
    // Actual I/O is at most the page wire budget plus one 12-byte lookahead header.
    Observation<DirectoryIndexScanPage> read_page(const IndexScanCursor &cursor, DirectoryIndexScanLimits limits = {},
                                                  std::stop_token stop = {});
    DirectoryIndexStoreState state() const noexcept {
        return state_;
    }
    std::uint64_t rows() const noexcept {
        return rows_;
    }
    std::uint64_t bytes() const noexcept {
        return bytes_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }

  private:
    friend class DirectoryIndexSortJob;
    DirectoryIndexStore(std::unique_ptr<IndexByteFile> file, std::shared_ptr<IndexStorageBudget> budget,
                        DirectoryIndexContext context, DirectoryIndexStoreLimits limits,
                        std::shared_ptr<IndexFileFactory> factory);
    static Observation<std::unique_ptr<DirectoryIndexStore>>
    adopt_reserved(std::unique_ptr<IndexByteFile> file, std::shared_ptr<IndexStorageBudget> budget,
                   DirectoryIndexContext context, DirectoryIndexStoreLimits limits, IndexStorageBudget::FileSlot &slot,
                   std::shared_ptr<IndexFileFactory> factory, std::stop_token stop);
    void fail(UnavailableObservation failure);
    void exception() noexcept;
    Observation<bool> read_exact(std::uint64_t offset, std::span<std::byte> bytes, std::stop_token stop);
    Observation<bool> check_size(std::stop_token stop);
    std::shared_ptr<IndexFileFactory> factory_;
    std::unique_ptr<IndexByteFile> file_;
    std::shared_ptr<IndexStorageBudget> budget_;
    DirectoryIndexContext context_;
    DirectoryIndexStoreLimits limits_;
    AdapterScope scope_;
    AdapterScope row_scope_;
    // Only the sorter can create nonconsecutive runs from validated source rows.
    std::optional<std::uint64_t> original_row_limit_;
    DirectoryIndexStoreState state_ = DirectoryIndexStoreState::Writing;
    std::uint64_t rows_ = 0, bytes_ = 0, charged_ = 0;
    std::optional<UnavailableObservation> failure_;
};
} // namespace ck::files
