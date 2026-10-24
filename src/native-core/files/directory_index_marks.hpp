// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_lookup.hpp"
namespace ck::files {
class IndexedPaneMarkJob;
class DirectoryMarksRecountJob;
enum class DirectoryIndexMarksState { Writing, Ready, Failed };
struct IndexedPaneMarkCounts {
    std::uint64_t total = 0, visible = 0, hidden = 0;
};
// Immutable worker-owned disk bitmap. One checked 576-byte block covers 4,096
// original row IDs. No native operation authority or persistent/restart format.
// Null snapshot at the pane boundary means the exact all-clear state.
class DirectoryIndexMarks final {
  public:
    ~DirectoryIndexMarks();
    DirectoryIndexMarks(const DirectoryIndexMarks &) = delete;
    DirectoryIndexMarks &operator=(const DirectoryIndexMarks &) = delete;
    const AdapterScope &row_scope() const noexcept {
        return row_scope_;
    }
    const AdapterScope &revision() const noexcept {
        return revision_;
    }
    std::uint64_t rows() const noexcept {
        return rows_;
    }
    std::uint64_t marked() const noexcept {
        return marked_;
    }
    DirectoryIndexMarksState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<bool> contains(const IndexRowKey &key);

  private:
    friend class IndexedPaneMarkJob;
    friend class DirectoryMarksRecountJob;
    friend class IndexedPaneSourceJob;
    using Bits = std::array<std::byte, 512>;
    DirectoryIndexMarks(std::unique_ptr<IndexByteFile> file, std::shared_ptr<IndexFileFactory> factory,
                        std::shared_ptr<IndexStorageBudget> budget, AdapterScope scope,
                        std::array<std::byte, 16> generation, std::uint64_t rows, std::uint64_t bytes);
    Observation<Bits> read_block(std::uint64_t block);
    Observation<bool> write_block(std::uint64_t block, const Bits &bits, std::stop_token stop);
    Observation<bool> size_matches();
    void fail(UnavailableObservation detail);
    void exception() noexcept;
    static bool bit(const Bits &bits, std::size_t at) noexcept;
    static void set(Bits &bits, std::size_t at, bool value) noexcept;
    static std::uint64_t count(const Bits &bits) noexcept;
    std::shared_ptr<IndexFileFactory> factory_;
    std::shared_ptr<IndexStorageBudget> budget_;
    AdapterScope row_scope_, revision_;
    std::array<std::byte, 16> generation_;
    const std::uint64_t rows_, bytes_;
    std::unique_ptr<IndexByteFile> file_;
    std::uint64_t marked_ = 0;
    DirectoryIndexMarksState state_ = DirectoryIndexMarksState::Writing;
    std::optional<UnavailableObservation> failure_;
};
// Recounts an immutable mark snapshot against a prepared view with bounded
// block/row work. This does not modify the mark file or revoke healthy inputs
// when the recount is cancelled.
enum class MarksRecountState { Running, Complete, Failed, Cancelled };
class DirectoryMarksRecountJob final {
  public:
    DirectoryMarksRecountJob(std::shared_ptr<DirectoryIndexMarks> marks, std::shared_ptr<DirectoryIndexLookup> view,
                             std::size_t rows_per_step = 256);
    MarksRecountState advance(std::stop_token stop = {});
    MarksRecountState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<IndexedPaneMarkCounts> completed() const;

  private:
    void fail(UnavailableObservation detail);
    std::shared_ptr<DirectoryIndexMarks> marks_;
    std::shared_ptr<DirectoryIndexLookup> view_;
    std::optional<DirectoryIndexMarks::Bits> bits_;
    std::uint64_t block_ = 0, seen_ = 0, visible_ = 0;
    std::size_t at_ = 0, step_;
    MarksRecountState state_ = MarksRecountState::Running;
    std::optional<UnavailableObservation> failure_;
    IndexedPaneMarkCounts result_;
};
} // namespace ck::files
