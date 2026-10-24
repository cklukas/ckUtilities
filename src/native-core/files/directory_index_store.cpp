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
class IndexStorageBudget::FileSlot final {
  public:
    explicit FileSlot(std::shared_ptr<IndexStorageBudget> budget) : budget_(std::move(budget)) {
        if (budget_->used_files_ != budget_->max_files_) {
            ++budget_->used_files_;
            owned_ = true;
        }
    }
    ~FileSlot() {
        if (owned_)
            --budget_->used_files_;
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
    bool owned_ = false;
};
namespace {
Observation<bool> configuration(const DirectoryIndexContext &context, DirectoryIndexStoreLimits limits) {
    const auto &r = limits.record;
    if (!limits.rows || !limits.bytes ||
        limits.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || r.record_bytes < 16 ||
        r.record_bytes > r.hard_record_bytes || !r.name_bytes || r.name_bytes > r.hard_name_bytes ||
        r.diagnostic_bytes > r.hard_diagnostic_bytes)
        return unavailable<bool>(error(AdapterError::InvalidLimit, "index_store.limits"));
    if (!context.source_scope.is_valid() ||
        std::all_of(context.generation.begin(), context.generation.end(), [](auto b) { return b == std::byte{}; }))
        return unavailable<bool>(error(AdapterError::InvalidIdentifier, "index_store.context"));
    return Observation<bool>::available(true);
}
} // namespace
DirectoryIndexStore::DirectoryIndexStore(std::unique_ptr<IndexByteFile> file,
                                         std::shared_ptr<IndexStorageBudget> budget, DirectoryIndexContext context,
                                         DirectoryIndexStoreLimits limits, std::shared_ptr<IndexFileFactory> factory)
    : factory_(std::move(factory)), file_(std::move(file)), budget_(std::move(budget)), context_(std::move(context)),
      limits_(limits) {
}
DirectoryIndexStore::~DirectoryIndexStore() {
    // Keep both reservations until the file and retained factory are retired.
    file_.reset();
    factory_.reset();
    budget_->used_bytes_ -= charged_;
    --budget_->used_files_;
}
Observation<std::unique_ptr<DirectoryIndexStore>>
DirectoryIndexStore::adopt_reserved(std::unique_ptr<IndexByteFile> file, std::shared_ptr<IndexStorageBudget> budget,
                                    DirectoryIndexContext context, DirectoryIndexStoreLimits limits,
                                    IndexStorageBudget::FileSlot &slot, std::shared_ptr<IndexFileFactory> factory,
                                    std::stop_token stop) {
    using Pointer = std::unique_ptr<DirectoryIndexStore>;
    if (!file)
        return unavailable<Pointer>(error(AdapterError::InvalidNativeResult, "index_store.null_file"));
    if (stop.stop_requested())
        return unavailable<Pointer>(cancelled());
    const auto size = file->size();
    if (!size.value())
        return unavailable<Pointer>(*size.unavailable_detail());
    if (*size.value() != 0)
        return unavailable<Pointer>(error(AdapterError::InvalidNativeResult, "index_store.nonempty"));
    if (stop.stop_requested())
        return unavailable<Pointer>(cancelled());
    auto owner = Pointer(
        new DirectoryIndexStore(std::move(file), std::move(budget), std::move(context), limits, std::move(factory)));
    slot.transfer();
    return Observation<Pointer>::available(std::move(owner));
}
Observation<std::unique_ptr<DirectoryIndexStore>>
DirectoryIndexStore::create_new(std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<IndexStorageBudget> budget,
                                DirectoryIndexContext context, DirectoryIndexStoreLimits limits, std::stop_token stop) {
    using Pointer = std::unique_ptr<DirectoryIndexStore>;
    if (!factory || !budget)
        return unavailable<Pointer>(error(AdapterError::InvalidLimit, "index_store.limits"));
    auto valid = configuration(context, limits);
    if (!valid.value())
        return unavailable<Pointer>(*valid.unavailable_detail());
    if (stop.stop_requested())
        return unavailable<Pointer>(cancelled());
    IndexStorageBudget::FileSlot slot(budget);
    if (!slot.acquired())
        return unavailable<Pointer>(exhausted());
    auto file = factory->create_file();
    if (!file.value())
        return unavailable<Pointer>(*file.unavailable_detail());
    return adopt_reserved(std::move(file).take_value(), std::move(budget), std::move(context), limits, slot, factory,
                          stop);
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
        auto checked = check_size(stop);
        if (!checked.value())
            return checked;
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
        auto filled = read_exact(address.offset_, wire, stop);
        if (!filled.value())
            return unavailable<DirectoryIndexRecord>(*filled.unavailable_detail());
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
Observation<bool> DirectoryIndexStore::read_exact(std::uint64_t offset, std::span<std::byte> bytes,
                                                  std::stop_token stop) {
    std::size_t done = 0;
    while (done != bytes.size()) {
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<bool>(*failure_);
        }
        auto got = file_->read_at(offset + done, bytes.subspan(done));
        if (!got.value()) {
            fail(*got.unavailable_detail());
            return unavailable<bool>(*failure_);
        }
        if (!*got.value() || *got.value() > bytes.size() - done) {
            fail(error(AdapterError::InvalidNativeResult, "index_store.read_count"));
            return unavailable<bool>(*failure_);
        }
        done += *got.value();
    }
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
    return Observation<bool>::available(true);
}
Observation<bool> DirectoryIndexStore::check_size(std::stop_token stop) {
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
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
    return Observation<bool>::available(true);
}
Observation<IndexScanCursor> DirectoryIndexStore::begin_scan() const {
    if (state_ != DirectoryIndexStoreState::Sealed)
        return unavailable<IndexScanCursor>(failure_.value_or(invalid_state()));
    return Observation<IndexScanCursor>::available(IndexScanCursor(scope_, 0, 0));
}
Observation<DirectoryIndexScanPage>
DirectoryIndexStore::read_page(const IndexScanCursor &cursor, DirectoryIndexScanLimits limits, std::stop_token stop) {
    using Result = Observation<DirectoryIndexScanPage>;
    if (state_ != DirectoryIndexStoreState::Sealed)
        return unavailable<DirectoryIndexScanPage>(failure_.value_or(invalid_state()));
    if (!limits.rows || limits.rows > limits.hard_rows || limits.wire_bytes < 16 ||
        limits.wire_bytes > limits.hard_wire_bytes)
        return unavailable<DirectoryIndexScanPage>(error(AdapterError::InvalidLimit, "index_store.scan_limits"));
    if (cursor.owner_ != scope_ || cursor.position_ > rows_ || cursor.offset_ > bytes_ ||
        ((cursor.position_ == rows_) != (cursor.offset_ == bytes_)))
        return unavailable<DirectoryIndexScanPage>(error(AdapterError::InvalidIdentifier, "index_store.scan_cursor"));
    try {
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<DirectoryIndexScanPage>(*failure_);
        }
        std::vector<DirectoryIndexScanRow> output;
        std::uint64_t position = cursor.position_, offset = cursor.offset_;
        std::size_t accumulated = 0;
        while (position != rows_ && output.size() != limits.rows) {
            if (bytes_ - offset < 16) {
                fail(error(AdapterError::InvalidNativeResult, "index_store.scan_length"));
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            std::array<std::byte, 12> header{};
            auto filled = read_exact(offset, header, stop);
            if (!filled.value())
                return unavailable<DirectoryIndexScanPage>(*filled.unavailable_detail());
            const auto framed = detail::directory_index_record_wire_size(header, limits_.record);
            if (!framed.value()) {
                fail(*framed.unavailable_detail());
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            const auto count = *framed.value();
            if (count > bytes_ - offset) {
                fail(error(AdapterError::InvalidNativeResult, "index_store.scan_length"));
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            if (count > limits.wire_bytes - accumulated) {
                if (output.empty())
                    return unavailable<DirectoryIndexScanPage>(
                        error(AdapterError::InventoryLimit, "index_store.scan_page_budget", ObservationState::Unknown));
                break;
            }
            std::vector<std::byte> wire(static_cast<std::size_t>(count));
            std::copy(header.begin(), header.end(), wire.begin());
            filled = read_exact(offset + header.size(), std::span(wire).subspan(header.size()), stop);
            if (!filled.value())
                return unavailable<DirectoryIndexScanPage>(*filled.unavailable_detail());
            auto decoded = decode_directory_index_record(wire, context_, limits_.record);
            if (!decoded.value()) {
                fail(*decoded.unavailable_detail());
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            if (decoded.value()->row_id != position + 1) {
                fail(error(AdapterError::InvalidNativeResult, "index_store.scan_row"));
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            output.push_back({IndexRecordAddress(scope_, position + 1, offset, static_cast<std::size_t>(count)),
                              std::move(decoded).take_value()});
            offset += count;
            ++position;
            accumulated += static_cast<std::size_t>(count);
        }
        const bool end = position == rows_;
        if (end) {
            if (offset != bytes_) {
                fail(error(AdapterError::InvalidNativeResult, "index_store.scan_end"));
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            auto checked = check_size(stop);
            if (!checked.value())
                return unavailable<DirectoryIndexScanPage>(*checked.unavailable_detail());
        }
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<DirectoryIndexScanPage>(*failure_);
        }
        return Result::available({std::move(output), IndexScanCursor(scope_, position, offset), accumulated, end});
    } catch (...) {
        exception();
        throw;
    }
}
} // namespace ck::files
