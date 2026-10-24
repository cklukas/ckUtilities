// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_sort_job.hpp"
#include <algorithm>
#include <stdexcept>
namespace ck::files {
namespace {
struct SortStopped {};
UnavailableObservation error(AdapterError code, const char *operation,
                             ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
} // namespace
struct DirectoryIndexSortJob::Stream {
    std::unique_ptr<DirectoryIndexStore> store;
    IndexScanCursor cursor;
    std::optional<DirectoryIndexScanPage> page;
    std::size_t at = 0;
    bool end = false;
    Stream(std::unique_ptr<DirectoryIndexStore> value, IndexScanCursor start)
        : store(std::move(value)), cursor(std::move(start)) {
    }
};
struct DirectoryIndexSortJob::Merge {
    Stream a, b;
    std::unique_ptr<DirectoryIndexStore> output;
    std::uint64_t expected;
    std::optional<DirectoryIndexRecord> previous;
    Merge(Stream left, Stream right, std::unique_ptr<DirectoryIndexStore> target, std::uint64_t rows)
        : a(std::move(left)), b(std::move(right)), output(std::move(target)), expected(rows) {
    }
};
DirectoryIndexSortJob::DirectoryIndexSortJob(std::shared_ptr<DirectoryIndexStore> source, PaneOrder order,
                                             DirectoryIndexSortLimits limits)
    : source_(std::move(source)), order_(order), limits_(limits) {
    if (!valid_pane_order(order) || !limits.block.rows || limits.block.rows > limits.block.hard_rows ||
        limits.block.wire_bytes < 16 || limits.block.wire_bytes > limits.block.hard_wire_bytes || !limits.merge_rows ||
        limits.merge_rows > DirectoryIndexScanLimits::hard_rows)
        throw std::invalid_argument("invalid index sort limits/order");
    if (!source_ || source_->state() != DirectoryIndexStoreState::Sealed) {
        fail(error(AdapterError::InvalidAccess, "index_sort.source_state"));
        return;
    }
    auto cursor = source_->begin_scan();
    if (!cursor.value()) {
        fail(*cursor.unavailable_detail());
        return;
    }
    source_cursor_ = std::move(cursor).take_value();
}
DirectoryIndexSortJob::~DirectoryIndexSortJob() = default;
void DirectoryIndexSortJob::release() noexcept {
    merge_.reset();
    carry_.reset();
    for (auto &run : levels_)
        run.reset();
    source_cursor_.reset();
    source_.reset();
}
void DirectoryIndexSortJob::fail(UnavailableObservation detail, bool stopped) {
    state_ = stopped && detail.error.domain == ErrorDomain::Adapter &&
                     detail.error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead)
                 ? DirectoryIndexSortState::Cancelled
                 : DirectoryIndexSortState::Failed;
    failure_ = std::move(detail);
    release();
}
void DirectoryIndexSortJob::cancel() noexcept {
    state_ = DirectoryIndexSortState::Cancelled;
    release();
    try {
        failure_ = error(AdapterError::InterruptedRead, "index_sort.cancelled", ObservationState::Unknown);
    } catch (...) {
        failure_.reset();
    }
}
bool DirectoryIndexSortJob::abandon() noexcept {
    if (state_ == DirectoryIndexSortState::Abandoned || state_ == DirectoryIndexSortState::Failed ||
        state_ == DirectoryIndexSortState::Cancelled)
        return false;
    release();
    result_.reset();
    if (state_ != DirectoryIndexSortState::Failed && state_ != DirectoryIndexSortState::Cancelled)
        state_ = DirectoryIndexSortState::Abandoned;
    return true;
}
int DirectoryIndexSortJob::compare(const DirectoryIndexRecord &a, const DirectoryIndexRecord &b) const {
    const int order = compare_pane_entries(a.name, a.metadata.value(), b.name, b.metadata.value(), order_);
    return order ? order : a.row_id < b.row_id ? -1 : a.row_id > b.row_id ? 1 : 0;
}
std::unique_ptr<DirectoryIndexStore> DirectoryIndexSortJob::new_run(std::stop_token stop) {
    auto created =
        DirectoryIndexStore::create_new(source_->factory_, source_->budget_, source_->context_, source_->limits_, stop);
    if (!created.value()) {
        fail(*created.unavailable_detail(), stop.stop_requested());
        return {};
    }
    auto run = std::move(created).take_value();
    run->original_row_limit_ = source_->original_row_limit_.value_or(source_->rows());
    run->row_scope_ = source_->row_scope_;
    return run;
}
bool DirectoryIndexSortJob::generate(std::stop_token stop) {
    auto run = new_run(stop);
    if (!run)
        return false;
    // Do not poison the source merely because this projection was cancelled.
    auto batch = source_->read_page(*source_cursor_, limits_.block);
    if (!batch.value()) {
        fail(*batch.unavailable_detail());
        return false;
    }
    auto page = std::move(batch).take_value();
    if ((!page.end && page.rows.empty()) || page.next.position() < source_cursor_->position() ||
        page.next.position() - source_cursor_->position() != page.rows.size()) {
        fail(error(AdapterError::InvalidNativeResult, "index_sort.source_progress"));
        return false;
    }
    if (stop.stop_requested()) {
        cancel();
        return false;
    }
    std::sort(page.rows.begin(), page.rows.end(), [&](const auto &a, const auto &b) {
        if (stop.stop_requested())
            throw SortStopped{};
        return compare(a.record, b.record) < 0;
    });
    if (stop.stop_requested()) {
        cancel();
        return false;
    }
    for (const auto &row : page.rows) {
        auto appended = run->append(row.record, stop);
        if (!appended.value()) {
            fail(*appended.unavailable_detail(), stop.stop_requested());
            return false;
        }
    }
    auto sealed = run->seal(stop);
    if (!sealed.value()) {
        fail(*sealed.unavailable_detail(), stop.stop_requested());
        return false;
    }
    source_cursor_ = page.next;
    source_end_ = page.end;
    carry_ = std::move(run);
    level_ = 0;
    return true;
}
bool DirectoryIndexSortJob::start_merge(std::unique_ptr<DirectoryIndexStore> a, std::unique_ptr<DirectoryIndexStore> b,
                                        std::stop_token stop) {
    if (a->rows() > source_->rows() || b->rows() > source_->rows() - a->rows()) {
        fail(error(AdapterError::InvalidNativeResult, "index_sort.merge_count"));
        return false;
    }
    const auto expected = a->rows() + b->rows();
    auto ac = a->begin_scan(), bc = b->begin_scan();
    if (!ac.value()) {
        fail(*ac.unavailable_detail());
        return false;
    }
    if (!bc.value()) {
        fail(*bc.unavailable_detail());
        return false;
    }
    auto output = new_run(stop);
    if (!output)
        return false;
    merge_ = std::make_unique<Merge>(Stream(std::move(a), std::move(ac).take_value()),
                                     Stream(std::move(b), std::move(bc).take_value()), std::move(output), expected);
    return true;
}
const DirectoryIndexRecord *DirectoryIndexSortJob::head(Stream &stream, std::stop_token stop) {
    if (stream.page && stream.at == stream.page->rows.size()) {
        stream.end = stream.page->end;
        stream.cursor = stream.page->next;
        stream.page.reset();
        stream.at = 0;
    }
    if (stream.end)
        return nullptr;
    if (!stream.page) {
        auto read = stream.store->read_page(stream.cursor, limits_.block, stop);
        if (!read.value()) {
            fail(*read.unavailable_detail(), stop.stop_requested());
            return nullptr;
        }
        stream.page = std::move(read).take_value();
        if (stream.page->rows.empty()) {
            if (!stream.page->end) {
                fail(error(AdapterError::InvalidNativeResult, "index_sort.empty_page"));
                return nullptr;
            }
            stream.end = true;
            return nullptr;
        }
    }
    return &stream.page->rows[stream.at].record;
}
bool DirectoryIndexSortJob::merge_step(std::stop_token stop) {
    for (std::size_t i = 0; i < limits_.merge_rows; ++i) {
        if (stop.stop_requested()) {
            cancel();
            return false;
        }
        const auto *a = head(merge_->a, stop);
        if (state_ != DirectoryIndexSortState::Running)
            return false;
        const auto *b = head(merge_->b, stop);
        if (state_ != DirectoryIndexSortState::Running)
            return false;
        if (!a && !b) {
            if (merge_->output->rows() != merge_->expected) {
                fail(error(AdapterError::InvalidNativeResult, "index_sort.merge_count"));
                return false;
            }
            auto sealed = merge_->output->seal(stop);
            if (!sealed.value()) {
                fail(*sealed.unavailable_detail(), stop.stop_requested());
                return false;
            }
            carry_ = std::move(merge_->output);
            merge_.reset();
            return true;
        }
        const bool left = a && (!b || compare(*a, *b) < 0);
        const auto &record = left ? *a : *b;
        if (merge_->previous && compare(*merge_->previous, record) >= 0) {
            fail(error(AdapterError::InvalidNativeResult, "index_sort.order_or_duplicate"));
            return false;
        }
        auto appended = merge_->output->append(record, stop);
        if (!appended.value()) {
            fail(*appended.unavailable_detail(), stop.stop_requested());
            return false;
        }
        merge_->previous = record;
        if (left)
            ++merge_->a.at;
        else
            ++merge_->b.at;
    }
    return true;
}
DirectoryIndexSortState DirectoryIndexSortJob::advance(std::stop_token stop) {
    if (state_ != DirectoryIndexSortState::Running)
        return state_;
    try {
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        if (source_->state() != DirectoryIndexStoreState::Sealed) {
            fail(source_->failure().value_or(error(AdapterError::InvalidAccess, "index_sort.source_state")));
            return state_;
        }
        if (merge_) {
            merge_step(stop);
            return state_;
        }
        if (carry_ && !finalizing_) {
            if (level_ == levels_.size()) {
                fail(error(AdapterError::InventoryLimit, "index_sort.levels"));
                return state_;
            }
            if (!levels_[level_])
                levels_[level_] = std::move(carry_);
            else {
                auto a = std::move(levels_[level_]);
                auto b = std::move(carry_);
                ++level_;
                start_merge(std::move(a), std::move(b), stop);
            }
            return state_;
        }
        if (!source_end_ && source_->rows() == 0) {
            auto checked = source_->read_page(*source_cursor_, limits_.block);
            if (!checked.value()) {
                fail(*checked.unavailable_detail());
                return state_;
            }
            if (stop.stop_requested()) {
                cancel();
                return state_;
            }
            result_ = source_;
            state_ = DirectoryIndexSortState::Complete;
            release();
            return state_;
        }
        if (!source_end_) {
            generate(stop);
            return state_;
        }
        finalizing_ = true;
        for (auto &run : levels_) {
            if (!run)
                continue;
            if (!carry_)
                carry_ = std::move(run);
            else {
                start_merge(std::move(carry_), std::move(run), stop);
                return state_;
            }
        }
        if (!carry_ || carry_->rows() != source_->rows() || carry_->bytes() != source_->bytes()) {
            fail(error(AdapterError::InvalidNativeResult, "index_sort.final_count"));
            return state_;
        }
        result_ = std::shared_ptr<DirectoryIndexStore>(std::move(carry_));
        state_ = DirectoryIndexSortState::Complete;
        release();
        return state_;
    } catch (const SortStopped &) {
        cancel();
        return state_;
    } catch (...) {
        state_ = DirectoryIndexSortState::Failed;
        failure_.reset();
        release();
        throw;
    }
}
Observation<std::shared_ptr<DirectoryIndexStore>> DirectoryIndexSortJob::completed() const {
    if (state_ == DirectoryIndexSortState::Complete && result_)
        return Observation<std::shared_ptr<DirectoryIndexStore>>::available(result_);
    const auto e = failure_.value_or(error(AdapterError::InvalidAccess, "index_sort.not_complete"));
    return Observation<std::shared_ptr<DirectoryIndexStore>>::unavailable(e.state, e.error);
}
} // namespace ck::files
