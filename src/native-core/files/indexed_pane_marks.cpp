// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "indexed_pane_marks.hpp"
#include "indexed_pane.hpp"
#include "index_storage_slot.hpp"
#include <algorithm>
#include <limits>
namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *op) {
    return {ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
}
} // namespace
IndexedPaneMarkJob::IndexedPaneMarkJob(std::shared_ptr<const CompletedIndexedPaneData> data,
                                       std::shared_ptr<DirectoryIndexMarks> previous, std::uint64_t visible,
                                       PaneStateStamp stamp, const IndexedMarkCommand &command, std::size_t step,
                                       std::uint64_t max_bytes)
    : data_(std::move(data)), previous_(std::move(previous)), stamp_(std::move(stamp)), command_(command),
      dataset_(data_->revision_), visible_(visible), step_(step) {
    if (!step_ || step_ > 4096 || !max_bytes ||
        max_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid mark limits");
    if (previous_)
        base_ = previous_->revision();
    if (command_.valueless_by_exception()) {
        fail(error(AdapterError::InvalidAccess, "marks.command"));
        return;
    }
    if (std::holds_alternative<IndexedClearMarks>(command_)) {
        visible_ = 0;
        return;
    }
    if (previous_ && (previous_->state() != DirectoryIndexMarksState::Ready || previous_->rows() != data_->rows() ||
                      previous_->row_scope() != data_->row_scope() || visible_ > previous_->marked())) {
        fail(previous_->failure().value_or(error(AdapterError::InvalidAccess, "marks.previous")));
        return;
    }
    if (!previous_ && visible_) {
        fail(error(AdapterError::InvalidNativeResult, "marks.visible_count"));
        return;
    }
    const auto locate = [&](const PaneRowId &id) -> std::optional<std::uint64_t> {
        auto rank = data_->lookup_->locate_row({id.scope, id.number});
        if (!rank.value()) {
            fail(*rank.unavailable_detail());
            return {};
        }
        if (!*rank.value()) {
            fail(error(AdapterError::InvalidIdentifier, "marks.excluded_target"));
            return {};
        }
        return **rank.value();
    };
    if (const auto *row = std::get_if<IndexedMarkRow>(&command_)) {
        auto rank = locate(row->row);
        if (!rank)
            return;
        low_ = *rank;
        high_ = *rank;
    }
    if (const auto *range = std::get_if<IndexedMarkRange>(&command_)) {
        auto a = locate(range->first);
        if (!a)
            return;
        auto b = locate(range->last);
        if (!b)
            return;
        low_ = std::min(*a, *b);
        high_ = std::max(*a, *b);
    }
    const auto rows = data_->rows();
    if (!rows)
        return;
    const auto blocks = rows / 4096 + (rows % 4096 != 0);
    if (blocks > max_bytes / 576) {
        fail(error(AdapterError::InventoryLimit, "marks.table_limit"));
        return;
    }
    const auto bytes = blocks * 576;
    auto factory = data_->store_->factory_;
    auto budget = data_->store_->budget_;
    IndexStorageBudget::FileSlot slot(budget, bytes);
    if (!slot.acquired()) {
        fail(error(AdapterError::InventoryLimit, "marks.budget"));
        return;
    }
    auto file = factory->create_file();
    if (!file.value()) {
        fail(*file.unavailable_detail());
        return;
    }
    if (!*file.value()) {
        fail(error(AdapterError::InvalidNativeResult, "marks.null_file"));
        return;
    }
    auto size = (*file.value())->size();
    if (!size.value()) {
        fail(*size.unavailable_detail());
        return;
    }
    if (*size.value() != 0) {
        fail(error(AdapterError::InvalidNativeResult, "marks.nonempty"));
        return;
    }
    auto owner = std::unique_ptr<DirectoryIndexMarks>(
        new DirectoryIndexMarks(std::move(file).take_value(), factory, budget, data_->row_scope(),
                                data_->store_->context_.generation, rows, bytes));
    slot.transfer();
    target_ = std::move(owner);
}
void IndexedPaneMarkJob::release() noexcept {
    bits_.reset();
    target_.reset();
    previous_.reset();
    data_.reset();
}
void IndexedPaneMarkJob::fail(UnavailableObservation detail) {
    state_ = IndexedMarkJobState::Failed;
    failure_ = std::move(detail);
    release();
}
void IndexedPaneMarkJob::cancel() noexcept {
    state_ = IndexedMarkJobState::Cancelled;
    release();
    try {
        failure_ = UnavailableObservation{
            ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "marks.cancelled"}};
    } catch (...) {
        failure_.reset();
    }
}
bool IndexedPaneMarkJob::abandon() noexcept {
    if (state_ == IndexedMarkJobState::Abandoned || state_ == IndexedMarkJobState::Failed ||
        state_ == IndexedMarkJobState::Cancelled)
        return false;
    release();
    result_.reset();
    state_ = IndexedMarkJobState::Abandoned;
    return true;
}
IndexedMarkJobState IndexedPaneMarkJob::advance(std::stop_token stop) {
    if (state_ != IndexedMarkJobState::Running)
        return state_;
    try {
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        if (std::holds_alternative<IndexedClearMarks>(command_) || !data_->rows()) {
            result_ = Result(new const CompletedIndexedMarkChange(stamp_, dataset_, base_, {}, {}));
            state_ = IndexedMarkJobState::Complete;
            release();
            return state_;
        }
        if (!bits_) {
            if (previous_) {
                auto read = previous_->read_block(block_);
                if (!read.value()) {
                    fail(*read.unavailable_detail());
                    return state_;
                }
                bits_ = std::move(read).take_value();
                previous_total_ += DirectoryIndexMarks::count(*bits_);
            } else
                bits_ = DirectoryIndexMarks::Bits{};
            at_ = 0;
        }
        const auto valid = static_cast<std::size_t>(std::min<std::uint64_t>(4096, data_->rows() - block_ * 4096));
        const auto change = [&](std::size_t at, bool value) -> bool {
            const bool old = DirectoryIndexMarks::bit(*bits_, at);
            if (old == value)
                return true;
            if (!value && !visible_) {
                auto detail = error(AdapterError::InvalidNativeResult, "marks.visible_underflow");
                if (previous_)
                    previous_->fail(detail);
                fail(std::move(detail));
                return false;
            }
            if (value)
                ++visible_;
            else
                --visible_;
            DirectoryIndexMarks::set(*bits_, at, value);
            return true;
        };
        if (const auto *one = std::get_if<IndexedMarkRow>(&command_)) {
            if ((one->row.number - 1) / 4096 == block_ &&
                !change(static_cast<std::size_t>((one->row.number - 1) % 4096), one->marked))
                return state_;
            at_ = valid;
        } else {
            for (std::size_t n = 0; at_ < valid && n < step_; ++n, ++at_) {
                if (stop.stop_requested()) {
                    cancel();
                    return state_;
                }
                auto located = data_->lookup_->locate_row({data_->row_scope(), block_ * 4096 + at_ + 1});
                if (!located.value()) {
                    fail(*located.unavailable_detail());
                    return state_;
                }
                if (!*located.value())
                    continue;
                const auto rank = **located.value();
                if (const auto *range = std::get_if<IndexedMarkRange>(&command_)) {
                    if (rank >= low_ && rank <= high_ && !change(at_, range->marked))
                        return state_;
                } else if (const auto *all = std::get_if<IndexedMarkVisible>(&command_)) {
                    if (!change(at_, all->marked))
                        return state_;
                } else if (!change(at_, !DirectoryIndexMarks::bit(*bits_, at_)))
                    return state_;
            }
        }
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        if (at_ != valid)
            return state_;
        auto wrote = target_->write_block(block_, *bits_, stop);
        if (!wrote.value()) {
            const auto &detail = *wrote.unavailable_detail();
            if (stop.stop_requested() && detail.error.domain == ErrorDomain::Adapter &&
                detail.error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead))
                cancel();
            else
                fail(detail);
            return state_;
        }
        total_ += DirectoryIndexMarks::count(*bits_);
        bits_.reset();
        ++block_;
        if (block_ == data_->rows() / 4096 + (data_->rows() % 4096 != 0)) {
            if ((previous_ && previous_total_ != previous_->marked()) || visible_ > total_ ||
                visible_ > data_->visible_rows()) {
                auto detail = error(AdapterError::InvalidNativeResult, "marks.counts");
                if (previous_)
                    previous_->fail(detail);
                fail(std::move(detail));
                return state_;
            }
            auto size = target_->size_matches();
            if (!size.value()) {
                fail(*size.unavailable_detail());
                return state_;
            }
            if (stop.stop_requested()) {
                cancel();
                return state_;
            }
            target_->marked_ = total_;
            target_->state_ = DirectoryIndexMarksState::Ready;
            if (!total_)
                target_.reset();
            result_ = Result(new const CompletedIndexedMarkChange(stamp_, dataset_, base_, target_,
                                                                  {total_, visible_, total_ - visible_}));
            state_ = IndexedMarkJobState::Complete;
            release();
        }
        return state_;
    } catch (...) {
        state_ = IndexedMarkJobState::Failed;
        failure_.reset();
        release();
        throw;
    }
}
Observation<IndexedPaneMarkJob::Result> IndexedPaneMarkJob::completed() const {
    if (state_ == IndexedMarkJobState::Complete && result_)
        return Observation<Result>::available(result_);
    if (failure_)
        return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
} // namespace ck::files
