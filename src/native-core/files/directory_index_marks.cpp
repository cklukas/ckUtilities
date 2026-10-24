// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_marks.hpp"
#include <algorithm>
#include <bit>
namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *op) {
    return {ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
}
template <class T> Observation<T> unavailable(const UnavailableObservation &e) {
    return Observation<T>::unavailable(e.state, e.error);
}
void put(std::span<std::byte> b, std::size_t at, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        b[at + i] = static_cast<std::byte>((v >> (i * 8)) & 255);
}
std::uint64_t get(std::span<const std::byte> b, std::size_t at, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
        v |= std::to_integer<std::uint64_t>(b[at + i]) << (i * 8);
    return v;
}
constexpr std::array<std::byte, 8> magic{std::byte{'C'}, std::byte{'K'}, std::byte{'F'}, std::byte{'M'},
                                         std::byte{'R'}, std::byte{'K'}, std::byte{'0'}, std::byte{'1'}};
} // namespace
DirectoryIndexMarks::DirectoryIndexMarks(std::unique_ptr<IndexByteFile> file, std::shared_ptr<IndexFileFactory> factory,
                                         std::shared_ptr<IndexStorageBudget> budget, AdapterScope scope,
                                         std::array<std::byte, 16> generation, std::uint64_t rows, std::uint64_t bytes)
    : factory_(std::move(factory)), budget_(std::move(budget)), row_scope_(std::move(scope)), generation_(generation),
      rows_(rows), bytes_(bytes), file_(std::move(file)) {
}
DirectoryIndexMarks::~DirectoryIndexMarks() {
    file_.reset();
    factory_.reset();
    budget_->used_bytes_ -= bytes_;
    --budget_->used_files_;
}
void DirectoryIndexMarks::fail(UnavailableObservation detail) {
    state_ = DirectoryIndexMarksState::Failed;
    failure_ = std::move(detail);
}
void DirectoryIndexMarks::exception() noexcept {
    state_ = DirectoryIndexMarksState::Failed;
    failure_.reset();
}
bool DirectoryIndexMarks::bit(const Bits &bits, std::size_t at) noexcept {
    return (std::to_integer<unsigned>(bits[at / 8]) & (1U << (at % 8))) != 0;
}
void DirectoryIndexMarks::set(Bits &bits, std::size_t at, bool value) noexcept {
    auto b = std::to_integer<unsigned>(bits[at / 8]);
    const auto mask = 1U << (at % 8);
    bits[at / 8] = static_cast<std::byte>(value ? b | mask : b & ~mask);
}
std::uint64_t DirectoryIndexMarks::count(const Bits &bits) noexcept {
    std::uint64_t n = 0;
    for (auto b : bits)
        n += static_cast<unsigned>(std::popcount(std::to_integer<unsigned>(b)));
    return n;
}
Observation<bool> DirectoryIndexMarks::size_matches() {
    auto size = file_->size();
    if (!size.value()) {
        fail(*size.unavailable_detail());
        return unavailable<bool>(*failure_);
    }
    if (*size.value() != bytes_) {
        fail(error(AdapterError::InvalidNativeResult, "marks.file_size"));
        return unavailable<bool>(*failure_);
    }
    return Observation<bool>::available(true);
}
Observation<DirectoryIndexMarks::Bits> DirectoryIndexMarks::read_block(std::uint64_t block) {
    if (state_ != DirectoryIndexMarksState::Ready)
        return unavailable<Bits>(failure_.value_or(error(AdapterError::InvalidAccess, "marks.state")));
    if (block >= bytes_ / 576)
        return unavailable<Bits>(error(AdapterError::InvalidIdentifier, "marks.block"));
    try {
        if (!size_matches().value())
            return unavailable<Bits>(*failure_);
        std::array<std::byte, 576> wire{};
        std::size_t done = 0;
        while (done < wire.size()) {
            auto read = file_->read_at(block * 576 + done, std::span<std::byte>(wire).subspan(done));
            if (!read.value()) {
                fail(*read.unavailable_detail());
                return unavailable<Bits>(*failure_);
            }
            if (!*read.value() || *read.value() > wire.size() - done) {
                fail(error(AdapterError::InvalidNativeResult, "marks.read_count"));
                return unavailable<Bits>(*failure_);
            }
            done += *read.value();
        }
        const auto valid = static_cast<std::size_t>(std::min<std::uint64_t>(4096, rows_ - block * 4096));
        if (!std::equal(magic.begin(), magic.end(), wire.begin()) ||
            !std::equal(generation_.begin(), generation_.end(), wire.begin() + 8) || get(wire, 24, 8) != block ||
            get(wire, 32, 8) != rows_ || get(wire, 40, 4) != valid ||
            std::any_of(wire.begin() + 44, wire.begin() + 60, [](auto b) { return b != std::byte{}; }) ||
            get(wire, 572, 4) != detail::directory_index_crc32c(std::span<const std::byte>(wire).first(572))) {
            fail(error(AdapterError::InvalidNativeResult, "marks.record"));
            return unavailable<Bits>(*failure_);
        }
        Bits bits;
        std::copy_n(wire.begin() + 60, bits.size(), bits.begin());
        for (std::size_t i = valid; i < 4096; ++i)
            if (bit(bits, i)) {
                fail(error(AdapterError::InvalidNativeResult, "marks.padding"));
                return unavailable<Bits>(*failure_);
            }
        return Observation<Bits>::available(bits);
    } catch (...) {
        exception();
        throw;
    }
}
Observation<bool> DirectoryIndexMarks::write_block(std::uint64_t block, const Bits &bits, std::stop_token stop) {
    if (state_ != DirectoryIndexMarksState::Writing || block >= bytes_ / 576)
        return unavailable<bool>(error(AdapterError::InvalidAccess, "marks.write_state"));
    std::array<std::byte, 576> wire{};
    std::copy(magic.begin(), magic.end(), wire.begin());
    std::copy(generation_.begin(), generation_.end(), wire.begin() + 8);
    put(wire, 24, block, 8);
    put(wire, 32, rows_, 8);
    put(wire, 40, std::min<std::uint64_t>(4096, rows_ - block * 4096), 4);
    std::copy(bits.begin(), bits.end(), wire.begin() + 60);
    put(wire, 572, detail::directory_index_crc32c(std::span<const std::byte>(wire).first(572)), 4);
    std::size_t done = 0;
    while (done < wire.size()) {
        if (stop.stop_requested())
            return Observation<bool>::unavailable(
                ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "marks.cancelled"});
        auto written = file_->write_at(block * 576 + done, std::span<const std::byte>(wire).subspan(done));
        if (!written.value()) {
            fail(*written.unavailable_detail());
            return unavailable<bool>(*failure_);
        }
        if (!*written.value() || *written.value() > wire.size() - done) {
            fail(error(AdapterError::InvalidNativeResult, "marks.write_count"));
            return unavailable<bool>(*failure_);
        }
        done += *written.value();
    }
    return Observation<bool>::available(true);
}
Observation<bool> DirectoryIndexMarks::contains(const IndexRowKey &key) {
    if (key.scope != row_scope_ || !key.number || key.number > rows_)
        return unavailable<bool>(error(AdapterError::InvalidIdentifier, "marks.key"));
    auto bits = read_block((key.number - 1) / 4096);
    if (!bits.value())
        return unavailable<bool>(*bits.unavailable_detail());
    return Observation<bool>::available(bit(*bits.value(), static_cast<std::size_t>((key.number - 1) % 4096)));
}
DirectoryMarksRecountJob::DirectoryMarksRecountJob(std::shared_ptr<DirectoryIndexMarks> marks,
                                                   std::shared_ptr<DirectoryIndexLookup> view, std::size_t step)
    : marks_(std::move(marks)), view_(std::move(view)), step_(step) {
    if (!step_ || step_ > 4096)
        throw std::invalid_argument("invalid mark recount step");
    if (marks_ && marks_->state() != DirectoryIndexMarksState::Ready) {
        fail(marks_->failure().value_or(error(AdapterError::InvalidAccess, "marks.recount_state")));
        return;
    }
    if (!marks_ || !view_ || marks_->row_scope() != view_->row_scope() || marks_->rows() != view_->total_rows())
        fail(error(AdapterError::InvalidAccess, "marks.recount_input"));
}
void DirectoryMarksRecountJob::fail(UnavailableObservation detail) {
    state_ = MarksRecountState::Failed;
    failure_ = std::move(detail);
    marks_.reset();
    view_.reset();
    bits_.reset();
}
MarksRecountState DirectoryMarksRecountJob::advance(std::stop_token stop) {
    if (state_ != MarksRecountState::Running)
        return state_;
    try {
        if (stop.stop_requested()) {
            state_ = MarksRecountState::Cancelled;
            marks_.reset();
            view_.reset();
            bits_.reset();
            return state_;
        }
        if (!bits_) {
            auto read = marks_->read_block(block_);
            if (!read.value()) {
                fail(*read.unavailable_detail());
                return state_;
            }
            bits_ = std::move(read).take_value();
            at_ = 0;
        }
        const auto valid = static_cast<std::size_t>(std::min<std::uint64_t>(4096, marks_->rows() - block_ * 4096));
        for (std::size_t n = 0; at_ < valid && n < step_; ++n, ++at_) {
            if (stop.stop_requested()) {
                state_ = MarksRecountState::Cancelled;
                marks_.reset();
                view_.reset();
                bits_.reset();
                return state_;
            }
            if (!DirectoryIndexMarks::bit(*bits_, at_))
                continue;
            auto located = view_->locate_row({marks_->row_scope(), block_ * 4096 + at_ + 1});
            if (!located.value()) {
                fail(*located.unavailable_detail());
                return state_;
            }
            ++seen_;
            if (*located.value())
                ++visible_;
        }
        if (stop.stop_requested()) {
            state_ = MarksRecountState::Cancelled;
            marks_.reset();
            view_.reset();
            bits_.reset();
            return state_;
        }
        if (at_ == valid) {
            bits_.reset();
            ++block_;
        }
        if (block_ == (marks_->rows() / 4096 + (marks_->rows() % 4096 != 0))) {
            if (seen_ != marks_->marked()) {
                auto detail = error(AdapterError::InvalidNativeResult, "marks.recount_total");
                marks_->fail(detail);
                fail(std::move(detail));
                return state_;
            }
            result_ = {seen_, visible_, seen_ - visible_};
            state_ = MarksRecountState::Complete;
            marks_.reset();
            view_.reset();
        }
        return state_;
    } catch (...) {
        state_ = MarksRecountState::Failed;
        failure_.reset();
        marks_.reset();
        view_.reset();
        bits_.reset();
        throw;
    }
}
Observation<IndexedPaneMarkCounts> DirectoryMarksRecountJob::completed() const {
    if (state_ == MarksRecountState::Complete)
        return Observation<IndexedPaneMarkCounts>::available(result_);
    if (failure_)
        return unavailable<IndexedPaneMarkCounts>(*failure_);
    return {};
}
} // namespace ck::files
