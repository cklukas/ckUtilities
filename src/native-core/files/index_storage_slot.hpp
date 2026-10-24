// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_store.hpp"
#include <algorithm>
namespace ck::files {
// Private quota-first RAII reservation shared by variable records and fixed
// lookup tables. Reservations survive partial writes until owner destruction.
class IndexStorageBudget::FileSlot final {
  public:
    explicit FileSlot(std::shared_ptr<IndexStorageBudget> budget, std::uint64_t bytes = 0)
        : budget_(std::move(budget)), bytes_(bytes) {
        if (budget_->used_files_ < budget_->max_files_ && bytes_ <= budget_->max_bytes_ - budget_->used_bytes_) {
            budget_->used_bytes_ += bytes_;
            budget_->peak_bytes_ = std::max(budget_->peak_bytes_, budget_->used_bytes_);
            ++budget_->used_files_;
            budget_->peak_files_ = std::max(budget_->peak_files_, budget_->used_files_);
            owned_ = true;
        }
    }
    ~FileSlot() {
        if (owned_) {
            --budget_->used_files_;
            budget_->used_bytes_ -= bytes_;
        }
    }
    FileSlot(const FileSlot &) = delete;
    FileSlot &operator=(const FileSlot &) = delete;
    bool acquired() const noexcept {
        return owned_;
    }
    void transfer() noexcept {
        owned_ = false;
    }

  private:
    std::shared_ptr<IndexStorageBudget> budget_;
    const std::uint64_t bytes_;
    bool owned_ = false;
};
} // namespace ck::files
