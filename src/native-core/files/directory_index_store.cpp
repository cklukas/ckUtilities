// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_store.hpp"
#include <algorithm>
#include <limits>

namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *op, ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
}
template <class T> Observation<T> unavailable(const UnavailableObservation &e) {
    return Observation<T>::unavailable(e.state, e.error);
}
UnavailableObservation exhausted() {
    return error(AdapterError::InventoryLimit, "index_store.budget", ObservationState::Unknown);
}
UnavailableObservation cancelled() {
    return error(AdapterError::InterruptedRead, "index_store.cancelled", ObservationState::Unknown);
}
UnavailableObservation invalid_state() {
    return error(AdapterError::InvalidAccess, "index_store.state");
}
} // namespace
IndexStorageBudget::IndexStorageBudget(std::uint64_t bytes, std::size_t files) : max_bytes_(bytes), max_files_(files) {
    if (!bytes || bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || !files)
        throw std::invalid_argument("invalid index storage budget");
}
DirectoryIndexStore::DirectoryIndexStore(std::unique_ptr<IndexByteFile> file,
                                         std::shared_ptr<IndexStorageBudget> budget, DirectoryIndexContext context,
                                         DirectoryIndexStoreLimits limits)
    : file_(std::move(file)), budget_(std::move(budget)), context_(std::move(context)), limits_(limits) {
    ++budget_->used_files_;
}
DirectoryIndexStore::~DirectoryIndexStore() {
    // Release the charge only after the backend closes/removes its storage.
    file_.reset();
    budget_->used_bytes_ -= charged_;
    --budget_->used_files_;
}
Observation<std::unique_ptr<DirectoryIndexStore>>
DirectoryIndexStore::create(std::unique_ptr<IndexByteFile> file, std::shared_ptr<IndexStorageBudget> budget,
                            DirectoryIndexContext context, DirectoryIndexStoreLimits limits) {
    using Result = Observation<std::unique_ptr<DirectoryIndexStore>>;
    const auto &r = limits.record;
    if (!file || !budget || !limits.rows || !limits.bytes ||
        limits.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || r.record_bytes < 16 ||
        r.record_bytes > r.hard_record_bytes || !r.name_bytes || r.name_bytes > r.hard_name_bytes ||
        r.diagnostic_bytes > r.hard_diagnostic_bytes)
        return unavailable<std::unique_ptr<DirectoryIndexStore>>(
            error(AdapterError::InvalidLimit, "index_store.limits"));
    if (!context.source_scope.is_valid() ||
        std::all_of(context.generation.begin(), context.generation.end(), [](auto b) { return b == std::byte{}; }))
        return unavailable<std::unique_ptr<DirectoryIndexStore>>(
            error(AdapterError::InvalidIdentifier, "index_store.context"));
    if (budget->used_files_ == budget->max_files_)
        return unavailable<std::unique_ptr<DirectoryIndexStore>>(exhausted());
    const auto size = file->size();
    if (!size.value())
        return unavailable<std::unique_ptr<DirectoryIndexStore>>(*size.unavailable_detail());
    if (*size.value() != 0)
        return unavailable<std::unique_ptr<DirectoryIndexStore>>(
            error(AdapterError::InvalidNativeResult, "index_store.nonempty"));
    return Result::available(std::unique_ptr<DirectoryIndexStore>(
        new DirectoryIndexStore(std::move(file), std::move(budget), std::move(context), limits)));
}
void DirectoryIndexStore::fail(UnavailableObservation failure) {
    state_ = DirectoryIndexStoreState::Failed;
    failure_ = std::move(failure);
}
void DirectoryIndexStore::exception() noexcept {
    state_ = DirectoryIndexStoreState::Failed;
    failure_.reset(); // the original exception carries the diagnostic
}
Observation<IndexRecordAddress> DirectoryIndexStore::append(const DirectoryIndexRecord &record, std::stop_token stop) {
    if (state_ != DirectoryIndexStoreState::Writing)
        return unavailable<IndexRecordAddress>(failure_.value_or(invalid_state()));
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<IndexRecordAddress>(*failure_);
    }
    if (rows_ == limits_.rows || rows_ == std::numeric_limits<std::uint64_t>::max())
        return unavailable<IndexRecordAddress>(exhausted());
    if (record.row_id != rows_ + 1)
        return unavailable<IndexRecordAddress>(error(AdapterError::InvalidIdentifier, "index_store.row"));
    // Encode before reservation/I/O. Invalid input and allocation failure leave
    // the existing store untouched. No RAM grows with the directory row count.
    auto encoded = encode_directory_index_record(record, context_, limits_.record);
    if (!encoded.value())
        return unavailable<IndexRecordAddress>(*encoded.unavailable_detail());
    const auto &wire = *encoded.value();
    if (wire.size() > limits_.bytes - bytes_ || wire.size() > budget_->max_bytes_ - budget_->used_bytes_)
        return unavailable<IndexRecordAddress>(exhausted());
    // Charge the complete attempted write. Failure may leave a partial file;
    // keep its reservation until backend destruction, never pretend rollback.
    budget_->used_bytes_ += wire.size();
    charged_ += wire.size();
    try {
        std::size_t done = 0;
        while (done != wire.size()) {
            if (stop.stop_requested()) {
                fail(cancelled());
                return unavailable<IndexRecordAddress>(*failure_);
            }
            auto wrote = file_->write_at(bytes_ + done, std::span(wire).subspan(done));
            if (!wrote.value()) {
                fail(*wrote.unavailable_detail());
                return unavailable<IndexRecordAddress>(*failure_);
            }
            if (!*wrote.value() || *wrote.value() > wire.size() - done) {
                fail(error(AdapterError::InvalidNativeResult, "index_store.write_count"));
                return unavailable<IndexRecordAddress>(*failure_);
            }
            done += *wrote.value();
        }
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<IndexRecordAddress>(*failure_);
        }
        const IndexRecordAddress address(scope_, record.row_id, bytes_, wire.size());
        bytes_ += wire.size();
        ++rows_;
        return Observation<IndexRecordAddress>::available(address);
    } catch (...) {
        exception();
        throw;
    }
}
Observation<bool> DirectoryIndexStore::seal(std::stop_token stop) {
    if (state_ == DirectoryIndexStoreState::Failed)
        return unavailable<bool>(failure_.value_or(invalid_state()));
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
    if (state_ == DirectoryIndexStoreState::Sealed)
        return Observation<bool>::available(true);
    try {
        const auto size = file_->size();
        if (!size.value()) {
            fail(*size.unavailable_detail());
            return unavailable<bool>(*failure_);
        }
        if (*size.value() != bytes_) {
            fail(error(AdapterError::InvalidNativeResult, "index_store.size"));
            return unavailable<bool>(*failure_);
        }
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<bool>(*failure_);
        }
        state_ = DirectoryIndexStoreState::Sealed;
        return Observation<bool>::available(true);
    } catch (...) {
        exception();
        throw;
    }
}
Observation<DirectoryIndexRecord> DirectoryIndexStore::read(const IndexRecordAddress &address, std::stop_token stop) {
    if (state_ != DirectoryIndexStoreState::Sealed)
        return unavailable<DirectoryIndexRecord>(failure_.value_or(invalid_state()));
    if (address.owner_ != scope_ || !address.row_ || address.row_ > rows_ || address.offset_ > bytes_ ||
        address.bytes_ > bytes_ - address.offset_ || address.bytes_ > limits_.record.record_bytes)
        return unavailable<DirectoryIndexRecord>(error(AdapterError::InvalidIdentifier, "index_store.address"));
    try {
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<DirectoryIndexRecord>(*failure_);
        }
        std::vector<std::byte> wire(address.bytes_);
        std::size_t done = 0;
        while (done != wire.size()) {
            if (stop.stop_requested()) {
                fail(cancelled());
                return unavailable<DirectoryIndexRecord>(*failure_);
            }
            auto got = file_->read_at(address.offset_ + done, std::span(wire).subspan(done));
            if (!got.value()) {
                fail(*got.unavailable_detail());
                return unavailable<DirectoryIndexRecord>(*failure_);
            }
            if (!*got.value() || *got.value() > wire.size() - done) {
                fail(error(AdapterError::InvalidNativeResult, "index_store.read_count"));
                return unavailable<DirectoryIndexRecord>(*failure_);
            }
            done += *got.value();
        }
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<DirectoryIndexRecord>(*failure_);
        }
        auto record = decode_directory_index_record(wire, context_, limits_.record);
        if (!record.value()) {
            fail(*record.unavailable_detail());
            return unavailable<DirectoryIndexRecord>(*failure_);
        }
        if (record.value()->row_id != address.row_) {
            fail(error(AdapterError::InvalidNativeResult, "index_store.row"));
            return unavailable<DirectoryIndexRecord>(*failure_);
        }
        return record;
    } catch (...) {
        exception();
        throw;
    }
}
} // namespace ck::files
