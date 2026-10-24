// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_completion_index.hpp"
#include <stdexcept>
namespace ck::vision {
namespace {
using namespace ck::files;
UnavailableObservation error(AdapterError code, const char *operation) {
    return {code == AdapterError::InventoryLimit ? ObservationState::Unknown : ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
} // namespace
bool FilesCompletionIndexJob::same_query(const FilesLocationCompletionQuery &a, const FilesLocationCompletionQuery &b) {
    return a.parent_ == b.parent_ && a.prefix_ == b.prefix_ && a.leading_ == b.leading_ && a.syntax_ == b.syntax_ &&
           a.separator_ == b.separator_ && a.limits_.components == b.limits_.components &&
           a.limits_.text_bytes == b.limits_.text_bytes &&
           a.limits_.path.max_native_bytes == b.limits_.path.max_native_bytes;
}
FilesCompletionIndexJob::FilesCompletionIndexJob(std::shared_ptr<const CompletedDirectoryIndex> source,
                                                 FilesLocationCompletionQuery query, FilesCompletionIndexLimits limits,
                                                 std::optional<FilesCompletionCursor> after)
    : source_(std::move(source)), limits_(limits) {
    if (!source_ || !query.parent().root_address().is_valid() || !limits.scan.rows ||
        limits.scan.rows > DirectoryIndexScanLimits::hard_rows || limits.scan.wire_bytes < 16 ||
        limits.scan.wire_bytes > DirectoryIndexScanLimits::hard_wire_bytes || !limits.candidates ||
        limits.candidates > 64 || !limits.text_bytes || limits.text_bytes > 1024 * 1024 || !limits.native_name_bytes ||
        limits.native_name_bytes > 1024 * 1024 || !limits.candidate_text_bytes || limits.candidate_text_bytes > 32768)
        throw std::invalid_argument("invalid completion index input/limits");
    staging_.index_scope = source_->store()->row_scope();
    if (after) {
        staging_.has_previous = true;
        if (after->scope_ != staging_.index_scope || !after->query_ || !same_query(*after->query_, query) ||
            after->limits_.scan.rows != limits.scan.rows || after->limits_.scan.wire_bytes != limits.scan.wire_bytes ||
            after->limits_.show_hidden != limits.show_hidden ||
            after->limits_.candidate_text_bytes != limits.candidate_text_bytes || after->skip_ >= limits.scan.rows) {
            fail(error(AdapterError::InvalidIdentifier, "completion_index.continuation"));
            return;
        }
        query_ = after->query_;
        cursor_ = after->scan_;
        skip_ = after->skip_;
    } else {
        query_ = std::make_shared<const FilesLocationCompletionQuery>(std::move(query));
        auto start = source_->store()->begin_scan();
        if (!start.value()) {
            fail(*start.unavailable_detail());
            return;
        }
        cursor_ = std::move(start).take_value();
    }
}
void FilesCompletionIndexJob::release() noexcept {
    source_.reset();
    query_.reset();
    cursor_.reset();
    staging_.entries.clear();
    staging_.next.reset();
}
void FilesCompletionIndexJob::fail(UnavailableObservation detail) {
    state_ = FilesCompletionIndexState::Failed;
    failure_ = std::move(detail);
    result_.reset();
    release();
}
void FilesCompletionIndexJob::cancel() noexcept {
    state_ = FilesCompletionIndexState::Cancelled;
    result_.reset();
    release();
}
bool FilesCompletionIndexJob::abandon() noexcept {
    if (state_ == FilesCompletionIndexState::Failed || state_ == FilesCompletionIndexState::Cancelled ||
        state_ == FilesCompletionIndexState::Abandoned)
        return false;
    result_.reset();
    release();
    state_ = FilesCompletionIndexState::Abandoned;
    return true;
}
void FilesCompletionIndexJob::finish(bool exhausted) {
    staging_.exhausted = exhausted;
    result_ = std::make_shared<const FilesCompletionPage>(std::move(staging_));
    state_ = FilesCompletionIndexState::Complete;
    release();
}
int FilesCompletionIndexJob::compare_prefix(const NativeName &name) const {
    if (const auto *prefix = query_->posix_prefix()) {
        if (!name.posix_bytes())
            throw std::invalid_argument("completion index/query encoding mismatch");
        return std::string_view(*name.posix_bytes()).substr(0, prefix->size()).compare(*prefix);
    }
    if (!name.windows_units())
        throw std::invalid_argument("completion index/query encoding mismatch");
    return std::u16string_view(*name.windows_units())
        .substr(0, query_->windows_prefix()->size())
        .compare(*query_->windows_prefix());
}
FilesCompletionIndexState FilesCompletionIndexJob::advance(std::stop_token stop) {
    if (state_ != FilesCompletionIndexState::Running)
        return state_;
    try {
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        // Complete a bounded read without passing stop: cancelling a read
        // invalidates its store. The shared healthy source must remain reusable.
        auto read = source_->store()->read_page(*cursor_, limits_.scan);
        if (!read.value()) {
            fail(*read.unavailable_detail());
            return state_;
        }
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        auto page = std::move(read).take_value();
        if (skip_ > page.rows.size() || (!page.end && page.rows.empty())) {
            fail(error(AdapterError::InvalidNativeResult, "completion_index.progress"));
            return state_;
        }
        for (auto at = skip_; at < page.rows.size(); ++at) {
            skip_ = at;
            if (stop.stop_requested()) {
                cancel();
                return state_;
            }
            auto &record = page.rows[at].record;
            if (record.name.encoding() != query_->parent().root_address().encoding()) {
                fail(error(AdapterError::InvalidIdentifier, "completion_index.encoding"));
                return state_;
            }
            const auto comparison = compare_prefix(record.name);
            if (comparison > 0) {
                finish(true); // Canonical name order proves no later prefix match.
                return state_;
            }
            ++staging_.examined;
            if (comparison < 0 || (!limits_.show_hidden && record.hidden.value() && *record.hidden.value()))
                continue;
            if (!record.metadata.value() || !record.metadata.value()->kind.value()) {
                ++staging_.unknown_kind;
                continue;
            }
            if (*record.metadata.value()->kind.value() != ObjectKind::Directory)
                continue; // Includes links/reparse points: no implicit following.
            auto proposal = query_->propose_directory(record.name);
            if (!proposal.value() || proposal.value()->text.size() > limits_.candidate_text_bytes) {
                ++staging_.unavailable_proposals;
                if (!staging_.first_proposal_error)
                    staging_.first_proposal_error =
                        proposal.value() ? error(AdapterError::InventoryLimit, "completion_index.candidate_text")
                                         : *proposal.unavailable_detail();
                continue;
            }
            const auto text_bytes = proposal.value()->text.size();
            const auto name_bytes = record.name.posix_bytes() ? record.name.posix_bytes()->size()
                                                              : record.name.windows_units()->size() * sizeof(char16_t);
            if (text_bytes > limits_.text_bytes - staging_.text_bytes ||
                name_bytes > limits_.native_name_bytes - staging_.native_name_bytes ||
                staging_.entries.size() == limits_.candidates) {
                if (staging_.entries.empty()) {
                    fail(error(AdapterError::InventoryLimit, "completion_index.page_bytes"));
                    return state_;
                }
                staging_.next = FilesCompletionCursor(staging_.index_scope, *cursor_, at, query_, limits_);
                finish(false); // A real additional eligible candidate was observed.
                return state_;
            }
            if (!limits_.show_hidden && !record.hidden.value())
                ++staging_.unknown_hidden;
            staging_.text_bytes += text_bytes;
            staging_.native_name_bytes += name_bytes;
            staging_.entries.push_back({record.row_id, std::move(record.name), std::move(proposal).take_value().text});
        }
        cursor_ = page.next;
        skip_ = 0;
        if (page.end)
            finish(true);
        return state_;
    } catch (...) {
        state_ = FilesCompletionIndexState::Failed;
        failure_.reset();
        result_.reset();
        release();
        throw;
    }
}
ck::files::Observation<FilesCompletionIndexJob::Result> FilesCompletionIndexJob::completed() const {
    if (state_ == FilesCompletionIndexState::Complete && result_)
        return Observation<Result>::available(result_);
    if (failure_)
        return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
} // namespace ck::vision
