// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_lookup.hpp"
#include "index_storage_slot.hpp"
#include <algorithm>
#include <limits>
namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *op, ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
}
UnavailableObservation cancelled() {
    return error(AdapterError::InterruptedRead, "index_lookup.cancelled", ObservationState::Unknown);
}
template <class T> Observation<T> unavailable(const UnavailableObservation &e) {
    return Observation<T>::unavailable(e.state, e.error);
}
void limits_valid(DirectoryIndexScanLimits limits) {
    if (!limits.rows || limits.rows > limits.hard_rows || limits.wire_bytes < 16 ||
        limits.wire_bytes > limits.hard_wire_bytes)
        throw std::invalid_argument("invalid lookup scan limits");
}
void put(std::span<std::byte> bytes, std::size_t at, std::uint64_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        bytes[at + i] = static_cast<std::byte>((value >> (i * 8)) & 255);
}
std::uint64_t get(std::span<const std::byte> bytes, std::size_t at, std::size_t n) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < n; ++i)
        result |= std::to_integer<std::uint64_t>(bytes[at + i]) << (i * 8);
    return result;
}
constexpr std::array<std::byte, 8> magic{std::byte{'C'}, std::byte{'K'}, std::byte{'F'}, std::byte{'L'},
                                         std::byte{'O'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'}};
} // namespace
DirectoryIndexLookup::DirectoryIndexLookup(std::unique_ptr<IndexByteFile> file,
                                           std::shared_ptr<DirectoryIndexStore> source,
                                           std::shared_ptr<IndexStorageBudget> budget, std::uint64_t bytes)
    : file_(std::move(file)), source_(std::move(source)), budget_(std::move(budget)), rows_(source_->rows()),
      bytes_(bytes) {
}
DirectoryIndexLookup::~DirectoryIndexLookup() {
    const bool owned = bool(file_);
    file_.reset(); // close/remove while source still pins its factory
    source_.reset();
    budget_->used_bytes_ -= bytes_;
    if (owned)
        --budget_->used_files_;
}
void DirectoryIndexLookup::fail(UnavailableObservation detail) {
    state_ = DirectoryIndexLookupState::Failed;
    failure_ = std::move(detail);
}
void DirectoryIndexLookup::exception() noexcept {
    state_ = DirectoryIndexLookupState::Failed;
    failure_.reset();
}
bool DirectoryIndexLookup::healthy() {
    if (state_ != DirectoryIndexLookupState::Ready)
        return false;
    if (source_->state() != DirectoryIndexStoreState::Sealed) {
        fail(source_->failure().value_or(error(AdapterError::InvalidAccess, "index_lookup.source_state")));
        return false;
    }
    return true;
}
Observation<bool> DirectoryIndexLookup::size_matches(std::stop_token stop) {
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
    if (!file_)
        return Observation<bool>::available(bytes_ == 0);
    auto size = file_->size();
    if (!size.value()) {
        fail(*size.unavailable_detail());
        return unavailable<bool>(*failure_);
    }
    if (*size.value() != bytes_) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.file_size"));
        return unavailable<bool>(*failure_);
    }
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
    return Observation<bool>::available(true);
}
Observation<bool> DirectoryIndexLookup::read_exact(std::uint64_t offset, std::span<std::byte> bytes,
                                                   std::stop_token stop) {
    if (!file_ || offset > bytes_ || bytes.size() > bytes_ - offset) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.read_bounds"));
        return unavailable<bool>(*failure_);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<bool>(*failure_);
        }
        auto read = file_->read_at(offset + done, bytes.subspan(done));
        if (!read.value()) {
            fail(*read.unavailable_detail());
            return unavailable<bool>(*failure_);
        }
        if (!*read.value() || *read.value() > bytes.size() - done) {
            fail(error(AdapterError::InvalidNativeResult, "index_lookup.read_count"));
            return unavailable<bool>(*failure_);
        }
        done += *read.value();
    }
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
    return Observation<bool>::available(true);
}
Observation<bool> DirectoryIndexLookup::write_exact(std::uint64_t offset, std::span<const std::byte> bytes,
                                                    std::stop_token stop) {
    if (!file_ || offset > bytes_ || bytes.size() > bytes_ - offset) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.write_bounds"));
        return unavailable<bool>(*failure_);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<bool>(*failure_);
        }
        auto written = file_->write_at(offset + done, bytes.subspan(done));
        if (!written.value()) {
            fail(*written.unavailable_detail());
            return unavailable<bool>(*failure_);
        }
        if (!*written.value() || *written.value() > bytes.size() - done) {
            fail(error(AdapterError::InvalidNativeResult, "index_lookup.write_count"));
            return unavailable<bool>(*failure_);
        }
        done += *written.value();
    }
    if (stop.stop_requested()) {
        fail(cancelled());
        return unavailable<bool>(*failure_);
    }
    return Observation<bool>::available(true);
}
std::array<std::byte, 64> DirectoryIndexLookup::encode(const Entry &e, std::uint32_t role) const {
    std::array<std::byte, 64> b{};
    std::copy(magic.begin(), magic.end(), b.begin());
    std::copy(source_->context_.generation.begin(), source_->context_.generation.end(), b.begin() + 8);
    put(b, 24, e.rank, 8);
    put(b, 32, e.row, 8);
    put(b, 40, e.offset, 8);
    put(b, 48, e.bytes, 4);
    put(b, 52, role, 4);
    put(b, 60, detail::directory_index_crc32c(std::span<const std::byte>(b).first(60)), 4);
    return b;
}
Observation<DirectoryIndexLookup::Entry> DirectoryIndexLookup::decode(std::span<const std::byte, 64> b,
                                                                      std::uint32_t role) const {
    if (!std::equal(magic.begin(), magic.end(), b.begin()) ||
        !std::equal(source_->context_.generation.begin(), source_->context_.generation.end(), b.begin() + 8) ||
        get(b, 52, 4) != role || get(b, 56, 4) != 0 || get(b, 60, 4) != detail::directory_index_crc32c(b.first(60)))
        return unavailable<Entry>(error(AdapterError::InvalidNativeResult, "index_lookup.record"));
    Entry e{get(b, 24, 8), get(b, 32, 8), get(b, 40, 8), static_cast<std::uint32_t>(get(b, 48, 4))};
    e.included = role == 1 || role == 2;
    if (e.rank >= rows_ || (e.included ? e.rank >= visible_ : e.rank < visible_) || !e.row || e.row > rows_ ||
        e.offset > source_->bytes() || e.bytes < 16 || e.bytes > source_->limits_.record.record_bytes ||
        e.bytes > source_->bytes() - e.offset)
        return unavailable<Entry>(error(AdapterError::InvalidNativeResult, "index_lookup.address"));
    return Observation<Entry>::available(e);
}
Observation<DirectoryIndexLookup::Entry> DirectoryIndexLookup::entry_at(std::uint64_t index, std::uint32_t role,
                                                                        std::stop_token stop) {
    std::array<std::byte, 64> bytes{};
    const auto offset = (role == 1 ? index : rows_ + index) * 64;
    if (!read_exact(offset, bytes, stop).value())
        return unavailable<Entry>(*failure_);
    const auto actual_role = static_cast<std::uint32_t>(get(bytes, 52, 4));
    if ((role == 1 && actual_role != 1) || (role == 2 && actual_role != 2 && actual_role != 3)) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.role"));
        return unavailable<Entry>(*failure_);
    }
    auto decoded = decode(bytes, actual_role);
    if (!decoded.value()) {
        fail(*decoded.unavailable_detail());
        return unavailable<Entry>(*failure_);
    }
    const auto e = *decoded.value();
    if ((role == 1 && e.rank != index) || (role == 2 && e.row != index + 1)) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.slot"));
        return unavailable<Entry>(*failure_);
    }
    const auto other_offset = (role == 1 ? rows_ + e.row - 1 : e.rank) * 64;
    if (!read_exact(other_offset, bytes, stop).value())
        return unavailable<Entry>(*failure_);
    auto other = decode(bytes, role == 1 ? 2 : e.included ? 1 : 4);
    if (!other.value()) {
        fail(*other.unavailable_detail());
        return unavailable<Entry>(*failure_);
    }
    const auto o = *other.value();
    if (e.rank != o.rank || e.row != o.row || e.offset != o.offset || e.bytes != o.bytes || e.included != o.included) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.cross_reference"));
        return unavailable<Entry>(*failure_);
    }
    return decoded;
}
Observation<std::optional<std::uint64_t>> DirectoryIndexLookup::locate_row(const IndexRowKey &key,
                                                                           std::stop_token stop) {
    if (state_ != DirectoryIndexLookupState::Ready)
        return unavailable<std::optional<std::uint64_t>>(
            failure_.value_or(error(AdapterError::InvalidAccess, "index_lookup.state")));
    if (key.scope != row_scope() || !key.number || key.number > rows_)
        return unavailable<std::optional<std::uint64_t>>(
            error(AdapterError::InvalidIdentifier, "index_lookup.row_key"));
    try {
        if (!healthy() || !size_matches(stop).value())
            return unavailable<std::optional<std::uint64_t>>(*failure_);
        auto entry = entry_at(key.number - 1, 2, stop);
        if (!entry.value())
            return unavailable<std::optional<std::uint64_t>>(*entry.unavailable_detail());
        return Observation<std::optional<std::uint64_t>>::available(
            entry.value()->included ? std::optional<std::uint64_t>(entry.value()->rank) : std::nullopt);
    } catch (...) {
        exception();
        throw;
    }
}
Observation<std::uint64_t> DirectoryIndexLookup::find_row(const IndexRowKey &key, std::stop_token stop) {
    auto located = locate_row(key, stop);
    if (!located.value())
        return unavailable<std::uint64_t>(*located.unavailable_detail());
    if (!*located.value())
        return unavailable<std::uint64_t>(
            error(AdapterError::NotApplicable, "index_lookup.filtered_row", ObservationState::Unknown));
    return Observation<std::uint64_t>::available(**located.value());
}
Observation<DirectoryIndexScanPage>
DirectoryIndexLookup::read_page(std::uint64_t first, DirectoryIndexScanLimits limits, std::stop_token stop) {
    limits_valid(limits);
    if (state_ != DirectoryIndexLookupState::Ready)
        return unavailable<DirectoryIndexScanPage>(
            failure_.value_or(error(AdapterError::InvalidAccess, "index_lookup.state")));
    if (first > visible_)
        return unavailable<DirectoryIndexScanPage>(error(AdapterError::InvalidIdentifier, "index_lookup.rank"));
    try {
        if (!healthy() || !size_matches(stop).value())
            return unavailable<DirectoryIndexScanPage>(*failure_);
        if (visible_ != rows_) {
            std::vector<DirectoryIndexScanRow> output;
            output.reserve(limits.rows);
            std::uint64_t next = first;
            std::size_t wire = 0;
            while (next < visible_ && output.size() < limits.rows) {
                auto found = entry_at(next, 1, stop);
                if (!found.value())
                    return unavailable<DirectoryIndexScanPage>(*found.unavailable_detail());
                const auto e = *found.value();
                if (e.bytes > limits.wire_bytes - wire) {
                    if (output.empty())
                        return unavailable<DirectoryIndexScanPage>(
                            error(AdapterError::InventoryLimit, "index_lookup.page_budget", ObservationState::Unknown));
                    break;
                }
                IndexRecordAddress address(source_->scope_, e.row, e.offset, e.bytes);
                auto row = source_->read(address);
                if (!row.value()) {
                    fail(*row.unavailable_detail());
                    return unavailable<DirectoryIndexScanPage>(*failure_);
                }
                if (stop.stop_requested()) {
                    fail(cancelled());
                    return unavailable<DirectoryIndexScanPage>(*failure_);
                }
                output.push_back({std::move(address), std::move(row).take_value()});
                wire += e.bytes;
                ++next;
            }
            if (next == visible_) {
                auto end = source_->read_page(IndexScanCursor(source_->scope_, rows_, source_->bytes()), {1, 16});
                if (!end.value()) {
                    fail(*end.unavailable_detail());
                    return unavailable<DirectoryIndexScanPage>(*failure_);
                }
            }
            if (stop.stop_requested()) {
                fail(cancelled());
                return unavailable<DirectoryIndexScanPage>(*failure_);
            }
            return Observation<DirectoryIndexScanPage>::available(
                {std::move(output), IndexScanCursor(scan_scope_, next, 0), wire, next == visible_});
        }
        std::optional<Entry> entry;
        if (first < rows_) {
            auto found = entry_at(first, 1, stop);
            if (!found.value())
                return unavailable<DirectoryIndexScanPage>(*found.unavailable_detail());
            entry = *found.value();
        }
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<DirectoryIndexScanPage>(*failure_);
        }
        // Projection cancellation must not revoke a healthy underlying store.
        auto page = source_->read_page(
            IndexScanCursor(source_->scope_, first, entry ? entry->offset : source_->bytes()), limits);
        if (!page.value()) {
            if (source_->state() != DirectoryIndexStoreState::Sealed)
                fail(*page.unavailable_detail());
            return unavailable<DirectoryIndexScanPage>(*page.unavailable_detail());
        }
        if (stop.stop_requested()) {
            fail(cancelled());
            return unavailable<DirectoryIndexScanPage>(*failure_);
        }
        const auto &p = *page.value();
        if (p.next.position() != first + p.rows.size() || p.end != (p.next.position() == rows_) ||
            (entry && (p.rows.empty() || p.rows.front().record.row_id != entry->row ||
                       p.rows.front().address.bytes_ != entry->bytes))) {
            fail(error(AdapterError::InvalidNativeResult, "index_lookup.source_position"));
            return unavailable<DirectoryIndexScanPage>(*failure_);
        }
        return page;
    } catch (...) {
        exception();
        throw;
    }
}
DirectoryIndexLookupJob::DirectoryIndexLookupJob(std::shared_ptr<DirectoryIndexStore> source,
                                                 DirectoryIndexScanLimits limits, std::uint64_t max_table_bytes,
                                                 std::stop_token stop, const PaneFilter &filter)
    : limits_(limits), matcher_(filter) {
    limits_valid(limits);
    if (!max_table_bytes || max_table_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid lookup byte limit");
    if (!source || source->state() != DirectoryIndexStoreState::Sealed ||
        source->original_row_limit_.value_or(source->rows()) != source->rows()) {
        fail(error(AdapterError::InvalidAccess, "index_lookup.source_state"));
        return;
    }
    if (stop.stop_requested()) {
        cancel();
        return;
    }
    if (source->rows() > max_table_bytes / 128) {
        fail(error(AdapterError::InventoryLimit, "index_lookup.table_limit", ObservationState::Unknown));
        return;
    }
    auto cursor = source->begin_scan();
    if (!cursor.value()) {
        fail(*cursor.unavailable_detail());
        return;
    }
    cursor_ = std::move(cursor).take_value();
    auto budget = source->budget_;
    const auto bytes = source->rows() * 128;
    if (!bytes) {
        index_ = Result(new DirectoryIndexLookup({}, std::move(source), budget, 0));
        return;
    }
    IndexStorageBudget::FileSlot slot(budget, bytes);
    if (!slot.acquired()) {
        fail(error(AdapterError::InventoryLimit, "index_lookup.budget", ObservationState::Unknown));
        return;
    }
    auto file = source->factory_->create_file();
    if (!file.value()) {
        fail(*file.unavailable_detail(), stop.stop_requested());
        return;
    }
    if (!*file.value()) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.null_file"));
        return;
    }
    if (stop.stop_requested()) {
        cancel();
        return;
    }
    auto size = (*file.value())->size();
    if (!size.value()) {
        fail(*size.unavailable_detail(), stop.stop_requested());
        return;
    }
    if (*size.value() != 0) {
        fail(error(AdapterError::InvalidNativeResult, "index_lookup.nonempty"));
        return;
    }
    if (stop.stop_requested()) {
        cancel();
        return;
    }
    auto owner = std::unique_ptr<DirectoryIndexLookup>(
        new DirectoryIndexLookup(std::move(file).take_value(), source, budget, bytes));
    slot.transfer();
    index_ = std::move(owner);
}
void DirectoryIndexLookupJob::release() noexcept {
    index_.reset();
    cursor_.reset();
}
void DirectoryIndexLookupJob::fail(UnavailableObservation detail, bool stopped) {
    state_ = stopped && detail.error.domain == ErrorDomain::Adapter &&
                     detail.error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead)
                 ? DirectoryIndexLookupJobState::Cancelled
                 : DirectoryIndexLookupJobState::Failed;
    failure_ = std::move(detail);
    release();
}
void DirectoryIndexLookupJob::cancel() noexcept {
    state_ = DirectoryIndexLookupJobState::Cancelled;
    release();
    try {
        failure_ = cancelled();
    } catch (...) {
        failure_.reset();
    }
}
bool DirectoryIndexLookupJob::abandon() noexcept {
    if (state_ == DirectoryIndexLookupJobState::Failed || state_ == DirectoryIndexLookupJobState::Cancelled ||
        state_ == DirectoryIndexLookupJobState::Abandoned)
        return false;
    release();
    result_.reset();
    state_ = DirectoryIndexLookupJobState::Abandoned;
    return true;
}
DirectoryIndexLookupJobState DirectoryIndexLookupJob::advance(std::stop_token stop) {
    if (state_ != DirectoryIndexLookupJobState::Initializing && state_ != DirectoryIndexLookupJobState::Building)
        return state_;
    try {
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        if (index_->source_->state() != DirectoryIndexStoreState::Sealed) {
            fail(index_->source_->failure().value_or(error(AdapterError::InvalidAccess, "index_lookup.source_state")));
            return state_;
        }
        if (state_ == DirectoryIndexLookupJobState::Initializing) {
            if (initialized_ < index_->bytes_) {
                const std::array<std::byte, 64 * 1024> zeros{};
                const auto n =
                    static_cast<std::size_t>(std::min<std::uint64_t>(zeros.size(), index_->bytes_ - initialized_));
                auto wrote = index_->write_exact(initialized_, std::span<const std::byte>(zeros).first(n), stop);
                if (!wrote.value()) {
                    fail(*wrote.unavailable_detail(), stop.stop_requested());
                    return state_;
                }
                initialized_ += n;
                return state_;
            }
            auto size = index_->size_matches(stop);
            if (!size.value()) {
                fail(*size.unavailable_detail(), stop.stop_requested());
                return state_;
            }
            state_ = DirectoryIndexLookupJobState::Building;
            return state_;
        }
        auto read = index_->source_->read_page(*cursor_, limits_);
        if (!read.value()) {
            fail(*read.unavailable_detail());
            return state_;
        }
        auto page = std::move(read).take_value();
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        for (const auto &row : page.rows) {
            const auto &a = row.address;
            const auto id = row.record.row_id;
            if (processed_ >= index_->rows_ || !id || id > index_->rows_ || a.owner_ != index_->source_->scope_ ||
                a.row_ != id || a.bytes_ > UINT32_MAX) {
                fail(error(AdapterError::InvalidNativeResult, "index_lookup.source_row"));
                return state_;
            }
            const auto id_offset = (index_->rows_ + id - 1) * 64;
            std::array<std::byte, 64> existing{};
            auto old = index_->read_exact(id_offset, existing, stop);
            if (!old.value()) {
                fail(*old.unavailable_detail(), stop.stop_requested());
                return state_;
            }
            if (std::any_of(existing.begin(), existing.end(), [](auto b) { return b != std::byte{}; })) {
                fail(error(AdapterError::InvalidNativeResult, "index_lookup.duplicate_row"));
                return state_;
            }
            const auto display = matcher_.needs_display() ? row.record.name.display_name() : std::string{};
            const auto match = matcher_.match(display, row.record.hidden, stop);
            if (match == PaneFilterMatch::Cancelled) {
                cancel();
                return state_;
            }
            const bool included = match == PaneFilterMatch::Included;
            const auto rank = included ? index_->visible_ : index_->rows_ - 1 - (processed_ - index_->visible_);
            DirectoryIndexLookup::Entry entry{rank, id, a.offset_, static_cast<std::uint32_t>(a.bytes_), included};
            for (const auto role : {1U, 2U}) {
                const auto wire = index_->encode(entry, included ? role : role == 1 ? 4 : 3);
                auto written = index_->write_exact(role == 1 ? rank * 64 : id_offset, wire, stop);
                if (!written.value()) {
                    fail(*written.unavailable_detail(), stop.stop_requested());
                    return state_;
                }
            }
            if (included)
                ++index_->visible_;
            ++processed_;
        }
        if (page.next.position() != processed_ || (!page.end && page.rows.empty())) {
            fail(error(AdapterError::InvalidNativeResult, "index_lookup.source_progress"));
            return state_;
        }
        cursor_ = page.next;
        if (page.end) {
            if (processed_ != index_->rows_) {
                fail(error(AdapterError::InvalidNativeResult, "index_lookup.source_count"));
                return state_;
            }
            auto size = index_->size_matches(stop);
            if (!size.value()) {
                fail(*size.unavailable_detail(), stop.stop_requested());
                return state_;
            }
            index_->state_ = DirectoryIndexLookupState::Ready;
            result_ = index_;
            state_ = DirectoryIndexLookupJobState::Complete;
            release();
        }
        return state_;
    } catch (...) {
        state_ = DirectoryIndexLookupJobState::Failed;
        failure_.reset();
        release();
        throw;
    }
}
Observation<DirectoryIndexLookupJob::Result> DirectoryIndexLookupJob::completed() const {
    if (state_ == DirectoryIndexLookupJobState::Complete && result_)
        return Observation<Result>::available(result_);
    if (failure_)
        return unavailable<Result>(*failure_);
    return {};
}
} // namespace ck::files
