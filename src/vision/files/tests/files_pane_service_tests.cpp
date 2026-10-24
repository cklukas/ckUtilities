// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_pane_service.hpp"
#include "native_index_file.hpp"
#include "scratch_directory.hpp"
#include "simulated_filesystem.hpp"
#include <cvision/testing/cktest.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <fstream>
namespace {
using namespace ck::files;
using namespace ck::vision;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("service fixture: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
NativeAbsolutePath address(const char *name) {
#ifdef _WIN32
    return take(NativeAbsolutePath::from_host(std::filesystem::path("V:/") / name));
#else
    return take(NativeAbsolutePath::from_host(std::filesystem::path("/") / name));
#endif
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    void wait() {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }))
            throw std::runtime_error("gate not entered");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct Trace {
    const std::thread::id ui = std::this_thread::get_id();
    std::atomic<unsigned> wrong_thread{0}, opens{0}, workspaces{0}, fs_closed{0}, factory_closed{0}, files_closed{0},
        wakes{0};
    std::atomic<bool> block_open{false}, block_read{false};
    std::atomic<int> startup_fault{0}, open_fault{0};
    std::atomic<unsigned> reads{0};
    std::atomic<bool> read_error{false};
    Gate open_gate, read_gate;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<UnavailableObservation> open_error, workspace_error;
    std::thread::id worker;
    void check() {
        std::lock_guard lock(mutex);
        const auto here = std::this_thread::get_id();
        if (worker == std::thread::id{})
            worker = here;
        if (here == ui || here != worker)
            ++wrong_thread;
    }
    void notify() {
        {
            std::lock_guard lock(mutex);
            ++wakes;
        }
        changed.notify_all();
    }
};
class Source final : public BasicFileSystem {
    std::shared_ptr<Trace> trace_;
    ck::testing::SimulatedFileSystem fs_;

  public:
    explicit Source(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {
        trace_->check();
        for (auto name : {"one", "two"}) {
            auto root = fs_.make_node(ObjectKind::Directory);
            fs_.bind(take(address(name).host_path()), root);
            for (int i = 0; i < 20; ++i)
                fs_.put(root, *NativeName::from_posix(std::string(name) + "-" + std::to_string(i)),
                        fs_.make_node(ObjectKind::RegularFile));
        }
    }
    ~Source() override {
        trace_->check();
        ++trace_->fs_closed;
    }
    Observation<std::unique_ptr<DirectoryCursor>> open_directory(const std::filesystem::path &path,
                                                                 DirectoryLinkPolicy policy) const override {
        trace_->check();
        ++trace_->opens;
        if (trace_->block_open.exchange(false))
            trace_->open_gate.block();
        if (trace_->open_fault == 1)
            throw std::bad_alloc();
        if (trace_->open_fault == 2)
            throw std::runtime_error("injected source exception");
        {
            std::lock_guard lock(trace_->mutex);
            if (trace_->open_error) {
                const auto &e = *trace_->open_error;
                return Observation<std::unique_ptr<DirectoryCursor>>::unavailable(e.state, e.error);
            }
        }
        return fs_.open_directory(path, policy);
    }
    Observation<BasicMetadata> inspect_basic(const std::filesystem::path &path) const override {
        trace_->check();
        return fs_.inspect_basic(path);
    }
};
class File final : public IndexByteFile {
    std::unique_ptr<IndexByteFile> file_;
    std::shared_ptr<Trace> trace_;

  public:
    File(std::unique_ptr<IndexByteFile> file, std::shared_ptr<Trace> trace)
        : file_(std::move(file)), trace_(std::move(trace)) {}
    ~File() override {
        trace_->check();
        ++trace_->files_closed;
    }
    Observation<std::uint64_t> size() override {
        trace_->check();
        return file_->size();
    }
    Observation<std::size_t> write_at(std::uint64_t at, std::span<const std::byte> data) override {
        trace_->check();
        return file_->write_at(at, data);
    }
    Observation<std::size_t> read_at(std::uint64_t at, std::span<std::byte> data) override {
        trace_->check();
        ++trace_->reads;
        if (trace_->block_read.exchange(false))
            trace_->read_gate.block();
        if (trace_->read_error.exchange(false))
            return Observation<std::size_t>::unavailable(ObservationState::Denied,
                                                         {ErrorDomain::Posix, 13, "cache.read.denied"});
        return file_->read_at(at, data);
    }
};
class Factory final : public IndexFileFactory {
    std::shared_ptr<IndexFileFactory> factory_;
    std::shared_ptr<Trace> trace_;

  public:
    Factory(std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<Trace> trace)
        : factory_(std::move(factory)), trace_(std::move(trace)) {}
    ~Factory() override {
        trace_->check();
        ++trace_->factory_closed;
    }
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        trace_->check();
        auto value = factory_->create_file();
        if (!value.value())
            return value;
        return Observation<std::unique_ptr<IndexByteFile>>::available(
            std::make_unique<File>(std::move(value).take_value(), trace_));
    }
};
struct Fixture {
    ck::testing::ScratchDirectory scratch{"files-service", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT};
    std::shared_ptr<Trace> trace = std::make_shared<Trace>();
    std::unique_ptr<FilesPaneService> service;
    Fixture() {
#ifndef _WIN32
        std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
    }
    ~Fixture() {
        trace->open_gate.release();
        trace->read_gate.release();
        if (service)
            service->join();
    }
    FilesPaneBackend backend() {
        FilesPaneBackend b;
        b.filesystem = [t = trace]() -> std::unique_ptr<BasicFileSystem> {
            t->check();
            if (t->startup_fault == 1)
                throw std::bad_alloc();
            if (t->startup_fault == 2)
                throw std::runtime_error("startup exception");
            return std::make_unique<Source>(t);
        };
        b.workspace = [t = trace, path = take(NativeAbsolutePath::from_host(
                                      scratch.path()))]() -> Observation<std::shared_ptr<IndexFileFactory>> {
            t->check();
            ++t->workspaces;
            {
                std::lock_guard lock(t->mutex);
                if (t->workspace_error) {
                    auto e = *t->workspace_error;
                    return Observation<std::shared_ptr<IndexFileFactory>>::unavailable(e.state, e.error);
                }
            }
            auto opened = open_native_index_workspace(path);
            if (!opened.value())
                return opened;
            return Observation<std::shared_ptr<IndexFileFactory>>::available(
                std::make_shared<Factory>(*opened.value(), t));
        };
        b.storage_model = StorageModel::LinuxStatx;
        return b;
    }
    void start(FilesPaneServiceLimits limits = {}) {
        service = std::make_unique<FilesPaneService>(backend(), limits, [t = trace] { t->notify(); });
    }
    FilesPaneReply reply() {
        std::optional<FilesPaneReply> value;
        std::unique_lock lock(trace->mutex);
        if (!trace->changed.wait_for(lock, std::chrono::seconds(5), [&] {
                value = service->take();
                return value.has_value();
            }))
            throw std::runtime_error("service reply timeout");
        return std::move(*value);
    }
    void idle() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (service->working() || trace->wakes == 0) {
            if (std::chrono::steady_clock::now() >= until)
                throw std::runtime_error("service did not become idle");
            std::unique_lock lock(trace->mutex);
            trace->changed.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
    void stopped() {
        service->join();
        CK_CHECK(trace->wrong_thread == 0 && !service->running());
        CK_CHECK(std::filesystem::is_empty(scratch.path()));
    }
};
} // namespace
CK_TEST(files_service_focus_marks_and_view_changes_preserve_row_identity_on_worker) {
    Fixture f;
    f.start();
    auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    const auto row = first.page->rows()[7].id;
    auto submitted = f.service->change(nav.ticket, first.stamp, FilesPaneFocusRow{row}, {0, 5});
    CK_CHECK(submitted.state == FilesPaneSubmission::Accepted);
    auto focused = f.reply();
    CK_CHECK(focused.request == submitted.ticket && focused.kind == FilesPaneReplyKind::Focus &&
             focused.change_outcome == FilesPaneChangeOutcome::Applied && focused.state == FilesPaneReplyState::Ready);
    CK_CHECK(!first.stamp.current() && focused.page->focused_position() == 7 && focused.page->first() == 3);
    CK_CHECK(focused.page->rows().back().id == row);
    CK_CHECK(f.service
                 ->change(nav.ticket, focused.stamp,
                          FilesPaneChangeMarks{IndexedMarkRange{row, first.page->rows()[9].id, true}}, {5, 10})
                 .state == FilesPaneSubmission::Accepted);
    auto marked = f.reply();
    CK_CHECK(marked.kind == FilesPaneReplyKind::Marks && marked.change_outcome == FilesPaneChangeOutcome::Applied);
    CK_CHECK(marked.page->selection().marks->total == 3 && marked.page->selection().marks->visible == 3);
    unsigned marks = 0;
    for (const auto &entry : marked.page->rows())
        marks += entry.marked ? 1U : 0U;
    CK_CHECK(marks == 3);
    PaneView view;
    view.descending = true;
    CK_CHECK(f.service->change(nav.ticket, marked.stamp, FilesPaneChangeView{view}, {0, 5}).state ==
             FilesPaneSubmission::Accepted);
    auto reordered = f.reply();
    CK_CHECK(reordered.kind == FilesPaneReplyKind::View && reordered.change_outcome == FilesPaneChangeOutcome::Applied);
    CK_CHECK(reordered.page->focused_position() == 12 && reordered.page->rows().back().id == row);
    CK_CHECK(reordered.page->selection().marks->total == 3 && reordered.page->selection().marks->hidden == 0);
    CK_CHECK(f.trace->opens == 1); // Reorder uses the retained dataset, not a new scan.
    f.stopped();
}
CK_TEST(files_service_filtering_reports_hidden_marks_until_they_are_explicitly_cleared) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 20);
    auto initial = f.reply();
    CK_CHECK(
        f.service->change(nav.ticket, initial.stamp, FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 20}).state ==
        FilesPaneSubmission::Accepted);
    auto marked = f.reply();
    CK_CHECK(marked.page->selection().marks->total == 20);
    PaneView filter;
    filter.display_substring = "one-0";
    CK_CHECK(f.service->change(nav.ticket, marked.stamp, FilesPaneChangeView{filter}, {19, 5}).state ==
             FilesPaneSubmission::Accepted);
    auto filtered = f.reply();
    CK_CHECK(filtered.state == FilesPaneReplyState::Ready && filtered.page->first() == 0 &&
             filtered.page->visible_rows() == 1);
    CK_CHECK(filtered.page->selection().state == PaneSourcesState::HiddenMarksNeedReview);
    CK_CHECK(filtered.page->selection().marks->visible == 1 && filtered.page->selection().marks->hidden == 19);
    CK_CHECK(f.service->change(nav.ticket, filtered.stamp, FilesPaneChangeMarks{IndexedClearMarks{}}, {0, 5}).state ==
             FilesPaneSubmission::Accepted);
    auto cleared = f.reply();
    CK_CHECK(cleared.page->selection().marks->total == 0 && !cleared.page->rows().front().marked);
    CK_CHECK(cleared.page->selection().state != PaneSourcesState::HiddenMarksNeedReview && f.trace->opens == 1);
    f.stopped();
}
CK_TEST(files_service_state_changes_reject_stale_stamps_and_foreign_rows_without_changing_selection) {
    Fixture f;
    f.start();
    auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    CK_CHECK(f.service->change(nav.ticket, first.stamp, FilesPaneFocusRow{first.page->rows()[3].id}, {0, 5}).state ==
             FilesPaneSubmission::Accepted);
    auto focused = f.reply();
    const auto reads = f.trace->reads.load();
    CK_CHECK(f.service->change(nav.ticket, first.stamp, FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 5}).state ==
             FilesPaneSubmission::Accepted);
    auto stale = f.reply();
    CK_CHECK(stale.state == FilesPaneReplyState::Stale && stale.change_outcome == FilesPaneChangeOutcome::NotApplied);
    CK_CHECK(!stale.page && f.trace->reads == reads && focused.stamp.current());
    Fixture other;
    other.start();
    other.service->navigate(address("two"));
    auto foreign = other.reply();
    CK_CHECK(
        f.service->change(nav.ticket, focused.stamp, FilesPaneFocusRow{foreign.page->rows()[0].id}, {0, 5}).state ==
        FilesPaneSubmission::Accepted);
    auto invalid = f.reply();
    CK_CHECK(invalid.state == FilesPaneReplyState::Failed &&
             invalid.change_outcome == FilesPaneChangeOutcome::NotApplied);
    CK_CHECK(invalid.page && invalid.page->focused_position() == 3 && invalid.page->selection().marks->total == 0);
    CK_CHECK(
        f.service->change(foreign.navigation, invalid.stamp, FilesPaneChangeMarks{IndexedClearMarks{}}, {0, 5}).state ==
        FilesPaneSubmission::Invalid);
    f.stopped();
    other.stopped();
}
CK_TEST(files_service_state_changes_are_acknowledged_and_cannot_be_replaced_by_pages_or_other_changes) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    f.trace->block_read = true;
    const auto command = FilesPaneChangeMarks{IndexedMarkRow{first.page->rows()[3].id, true}};
    auto submitted = f.service->change(nav.ticket, first.stamp, command, {0, 5});
    CK_CHECK(submitted.state == FilesPaneSubmission::Accepted);
    f.trace->read_gate.wait();
    for (unsigned i = 0; i < 100; ++i) {
        CK_CHECK(f.service->change(nav.ticket, first.stamp, command, {0, 5}).state == FilesPaneSubmission::Busy);
        CK_CHECK(f.service->read_page(nav.ticket, first.stamp, {5, 5}).state == FilesPaneSubmission::Busy);
    }
    f.trace->read_gate.release();
    f.idle();
    CK_CHECK(f.service->change(nav.ticket, first.stamp, command, {0, 5}).state == FilesPaneSubmission::Busy);
    auto result = f.reply();
    CK_CHECK(result.request == submitted.ticket && result.change_outcome == FilesPaneChangeOutcome::Applied);
    CK_CHECK(result.page->selection().marks->total == 1);
    f.stopped();
}
CK_TEST(files_service_cancelling_blocked_mark_preparation_retains_the_previous_selection) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    f.trace->block_read = true;
    auto submitted = f.service->change(nav.ticket, first.stamp,
                                       FilesPaneChangeMarks{IndexedMarkRow{first.page->rows()[3].id, true}}, {0, 5});
    f.trace->read_gate.wait();
    CK_CHECK(f.service->cancel(submitted.ticket) && f.service->working());
    f.trace->read_gate.release();
    auto result = f.reply();
    CK_CHECK(result.change_outcome == FilesPaneChangeOutcome::NotApplied);
    CK_CHECK(result.state == FilesPaneReplyState::Cancelled && !result.page);
    CK_CHECK(f.service->read_page(nav.ticket, result.stamp, {0, 5}).state == FilesPaneSubmission::Accepted);
    CK_CHECK(f.reply().page->selection().marks->total == 0);
    f.stopped();
}
CK_TEST(files_service_view_change_cancelled_during_blocked_cache_read_preserves_previous_order) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    f.trace->block_read = true;
    PaneView view;
    view.descending = true;
    auto submitted = f.service->change(nav.ticket, first.stamp, FilesPaneChangeView{view}, {0, 5});
    f.trace->read_gate.wait();
    CK_CHECK(f.service->cancel(submitted.ticket));
    f.trace->read_gate.release();
    auto result = f.reply();
    CK_CHECK(result.state == FilesPaneReplyState::Cancelled &&
             result.change_outcome == FilesPaneChangeOutcome::NotApplied);
    CK_CHECK(f.service->read_page(nav.ticket, result.stamp, {0, 5}).state == FilesPaneSubmission::Accepted);
    auto retained = f.reply();
    CK_CHECK(!retained.page->view().descending && retained.page->rows()[0].id == first.page->rows()[0].id);
    f.stopped();
}
CK_TEST(files_service_navigation_supersedes_a_blocked_state_change_without_replaying_it) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    f.trace->block_read = true;
    auto edit = f.service->change(nav.ticket, first.stamp,
                                  FilesPaneChangeMarks{IndexedMarkRow{first.page->rows()[3].id, true}}, {0, 5});
    f.trace->read_gate.wait();
    const auto next = f.service->navigate(address("two"), {}, 5);
    CK_CHECK(next.state == FilesPaneSubmission::Accepted && !f.service->cancel(edit.ticket));
    f.trace->read_gate.release();
    auto current = f.reply();
    CK_CHECK(current.request == next.ticket && current.kind == FilesPaneReplyKind::Navigation);
    CK_CHECK(*current.active_location == address("two") && current.page->selection().marks->total == 0);
    CK_CHECK(
        f.service->change(next.ticket, current.stamp, FilesPaneChangeMarks{IndexedInvertVisible{}}, {0, 5}).state ==
        FilesPaneSubmission::Accepted);
    auto inverted = f.reply();
    CK_CHECK(inverted.page->selection().marks->total == 20 && inverted.page->rows()[0].marked);
    CK_CHECK(
        f.service->change(next.ticket, inverted.stamp, FilesPaneChangeMarks{IndexedInvertVisible{}}, {0, 5}).state ==
        FilesPaneSubmission::Accepted);
    CK_CHECK(f.reply().page->selection().marks->total == 0);
    f.stopped();
}
CK_TEST(files_service_invalid_state_change_parameters_are_rejected_before_any_native_work) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 5);
    const auto first = f.reply();
    const auto reads = f.trace->reads.load();
    PaneView invalid;
    invalid.sort = static_cast<PaneSortKey>(999);
    CK_CHECK(f.service->change(nav.ticket, first.stamp, FilesPaneChangeView{invalid}, {0, 5}).state ==
             FilesPaneSubmission::Invalid);
    invalid.sort = PaneSortKey::Name;
    invalid.display_substring.assign(4097, 'x');
    CK_CHECK(f.service->change(nav.ticket, first.stamp, FilesPaneChangeView{invalid}, {0, 5}).state ==
             FilesPaneSubmission::LimitExceeded);
    for (auto count : {std::size_t{0}, std::size_t{4097}})
        CK_CHECK(
            f.service->change(nav.ticket, first.stamp, FilesPaneChangeMarks{IndexedClearMarks{}}, {0, count}).state ==
            FilesPaneSubmission::LimitExceeded);
    CK_CHECK(f.trace->reads == reads && first.stamp.current() && !f.service->working());
    f.stopped();
}
CK_TEST(files_service_notification_failure_after_mark_commit_reports_unknown_acknowledgment) {
    Fixture f;
    std::atomic<unsigned> attempts{0};
    f.service = std::make_unique<FilesPaneService>(f.backend(), FilesPaneServiceLimits{}, [&] {
        if (++attempts == 2)
            throw std::runtime_error("mark acknowledgment notification failed");
        f.trace->notify();
    });
    const auto nav = f.service->navigate(address("one"), {}, 5);
    auto first = f.reply();
    const auto submitted =
        f.service->change(nav.ticket, first.stamp, FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 5});
    {
        std::unique_lock lock(f.trace->mutex);
        if (!f.trace->changed.wait_for(lock, std::chrono::seconds(5), [&] { return !f.service->running(); }))
            throw std::runtime_error("faulted mark worker did not retire");
    }
    auto result = f.reply();
    CK_CHECK(result.request == submitted.ticket && result.state == FilesPaneReplyState::WorkerFault);
    CK_CHECK(result.kind == FilesPaneReplyKind::Fault && result.change_outcome == FilesPaneChangeOutcome::Unknown &&
             !result.page && !f.service->running());
    CK_CHECK(attempts == 3 && !first.stamp.current());
    f.stopped();
}
CK_TEST(files_service_committed_changes_remain_explicit_when_display_capture_exceeds_its_budget) {
    Fixture f;
    FilesPaneServiceLimits limits;
    limits.page_wire_bytes = 512;
    f.start(limits);
    const auto nav = f.service->navigate(address("one"), {}, 1);
    auto first = f.reply();
    CK_CHECK(
        f.service->change(nav.ticket, first.stamp, FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 20}).state ==
        FilesPaneSubmission::Accepted);
    auto marked = f.reply();
    CK_CHECK(marked.state == FilesPaneReplyState::LimitExceeded && !marked.page);
    CK_CHECK(marked.change_outcome == FilesPaneChangeOutcome::Applied && !first.stamp.current());
    CK_CHECK(f.service->read_page(nav.ticket, marked.stamp, {0, 1}).state == FilesPaneSubmission::Accepted);
    auto recovered = f.reply();
    CK_CHECK(recovered.page->selection().marks->total == 20 && recovered.page->rows()[0].marked);
    PaneView view;
    view.descending = true;
    CK_CHECK(f.service->change(nav.ticket, recovered.stamp, FilesPaneChangeView{view}, {0, 20}).state ==
             FilesPaneSubmission::Accepted);
    auto sorted = f.reply();
    CK_CHECK(sorted.state == FilesPaneReplyState::LimitExceeded && !sorted.page);
    CK_CHECK(sorted.change_outcome == FilesPaneChangeOutcome::Applied);
    CK_CHECK(f.service->read_page(nav.ticket, sorted.stamp, {0, 1}).state == FilesPaneSubmission::Accepted);
    auto final = f.reply();
    CK_CHECK(final.page->view().descending && final.page->rows()[0].display_name == "one-9");
    CK_CHECK(final.page->selection().marks->total == 20 && f.trace->opens == 1);
    f.stopped();
}
CK_TEST(files_service_native_mark_failure_keeps_its_exact_error_when_cancellation_races_it) {
    Fixture f;
    f.start();
    const auto nav = f.service->navigate(address("one"), {}, 20);
    auto first = f.reply();
    f.trace->block_read = true;
    f.trace->read_error = true;
    auto edit = f.service->change(nav.ticket, first.stamp,
                                  FilesPaneChangeMarks{IndexedMarkRow{first.page->rows()[3].id, true}}, {0, 5});
    f.trace->read_gate.wait();
    CK_CHECK(f.service->cancel(edit.ticket));
    f.trace->read_gate.release();
    auto result = f.reply();
    CK_CHECK(result.state == FilesPaneReplyState::Failed &&
             result.change_outcome == FilesPaneChangeOutcome::NotApplied);
    CK_CHECK(result.error && result.error->state == ObservationState::Denied);
    CK_CHECK(result.error->error.domain == ErrorDomain::Posix && result.error->error.code == 13 &&
             result.error->error.operation == "cache.read.denied");
    CK_CHECK(!result.page && f.service->running());
    f.stopped();
}
CK_TEST(files_service_native_index_navigation_and_pages_keep_all_native_io_on_worker) {
    Fixture f;
    f.start();
    auto nav = f.service->navigate(address("one"), {}, 5);
    CK_CHECK(nav.state == FilesPaneSubmission::Accepted);
    auto first = f.reply();
    CK_CHECK(first.request == nav.ticket && first.navigation == nav.ticket &&
             first.state == FilesPaneReplyState::Ready);
    CK_CHECK(first.page && first.page->rows().size() == 5 && first.page->visible_rows() == 20 &&
             *first.active_location == address("one"));
    auto read = f.service->read_page(nav.ticket, first.stamp, {10, 3});
    CK_CHECK(read.state == FilesPaneSubmission::Accepted);
    auto second = f.reply();
    CK_CHECK(second.request == read.ticket && second.page && second.page->first() == 10 &&
             second.page->rows().size() == 3);
    f.stopped();
    CK_CHECK(f.trace->fs_closed == 1 && f.trace->factory_closed == 1 && f.trace->files_closed > 0);
    CK_CHECK(!first.page->stamp().current() && first.page->rows().size() == 5);
}
CK_TEST(files_service_latest_navigation_supersedes_blocked_work_without_joining_or_queue_growth) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    auto old = f.service->navigate(address("one"));
    f.trace->open_gate.wait();
    FilesPaneSubmitted latest = old;
    for (int i = 0; i < 100; ++i)
        latest = f.service->navigate(address("two"), {}, 4);
    CK_CHECK(latest.state == FilesPaneSubmission::Accepted && f.trace->opens == 1 && f.service->working());
    CK_CHECK(!f.service->cancel(old.ticket));
    f.trace->open_gate.release();
    auto result = f.reply();
    CK_CHECK(result.request == latest.ticket && result.state == FilesPaneReplyState::Ready &&
             *result.active_location == address("two"));
    CK_CHECK(f.trace->opens == 2);
    f.stopped();
}
CK_TEST(files_service_failed_and_cancelled_navigation_preserve_active_location_and_page_stamp) {
    Fixture f;
    f.start();
    auto initial = f.service->navigate(address("one"));
    auto first = f.reply();
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "open.denied"}};
    }
    auto failed = f.service->navigate(address("two"));
    auto refusal = f.reply();
    CK_CHECK(refusal.request == failed.ticket && refusal.state == FilesPaneReplyState::Failed &&
             refusal.error->error.code == 13);
    CK_CHECK(*refusal.active_location == address("one") && *refusal.requested_location == address("two") &&
             first.stamp.current());
    auto read = f.service->read_page(failed.ticket, refusal.stamp, {0, 2});
    CK_CHECK(read.state == FilesPaneSubmission::Accepted && f.reply().page);
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error.reset();
    }
    f.trace->block_open = true;
    auto cancelled = f.service->navigate(address("two"));
    f.trace->open_gate.wait();
    CK_CHECK(f.service->cancel(cancelled.ticket));
    f.trace->open_gate.release();
    auto stopped = f.reply();
    CK_CHECK(stopped.state == FilesPaneReplyState::Cancelled && *stopped.active_location == address("one") &&
             first.stamp.current());
    f.stopped();
    (void)initial;
}
CK_TEST(files_service_native_refusal_wins_over_simultaneous_cancellation) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    auto nav = f.service->navigate(address("one"));
    f.trace->open_gate.wait();
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error =
            UnavailableObservation{ObservationState::Failed, {ErrorDomain::NtStatus, -41, "late.native.failure"}};
    }
    CK_CHECK(f.service->cancel(nav.ticket));
    f.trace->open_gate.release();
    auto result = f.reply();
    CK_CHECK(result.state == FilesPaneReplyState::Failed && result.error->error.domain == ErrorDomain::NtStatus &&
             result.error->error.code == -41);
    f.stopped();
}
CK_TEST(files_service_page_requests_coalesce_but_cannot_interrupt_navigation_or_unread_reply) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    auto nav = f.service->navigate(address("one"));
    f.trace->open_gate.wait();
    CK_CHECK(f.service->read_page(nav.ticket, {}, {}).state == FilesPaneSubmission::Busy);
    f.trace->open_gate.release();
    f.idle();
    CK_CHECK(f.service->read_page(nav.ticket, {}, {}).state == FilesPaneSubmission::Busy);
    auto first = f.reply();
    f.trace->block_read = true;
    auto read = f.service->read_page(nav.ticket, first.stamp, {0, 2});
    f.trace->read_gate.wait();
    auto latest = read;
    for (int i = 0; i < 50; ++i)
        latest = f.service->read_page(nav.ticket, first.stamp, {10, 4});
    f.trace->read_gate.release();
    auto last = f.reply();
    CK_CHECK(last.request == latest.ticket && last.state == FilesPaneReplyState::Ready && last.page->first() == 10);
    f.stopped();
}
CK_TEST(files_service_foreign_tickets_stamps_and_unresolved_ranges_do_not_become_current_rows) {
    Fixture a, b;
    a.start();
    b.start();
    auto ar = a.service->navigate(address("one"));
    auto br = b.service->navigate(address("two"));
    auto av = a.reply();
    auto bv = b.reply();
    CK_CHECK(ar.ticket.number() == br.ticket.number() && ar.ticket != br.ticket);
    CK_CHECK(a.service->read_page(br.ticket, av.stamp, {}).state == FilesPaneSubmission::Invalid);
    auto wrong = a.service->read_page(ar.ticket, bv.stamp, {});
    CK_CHECK(wrong.state == FilesPaneSubmission::Accepted && a.reply().state == FilesPaneReplyState::Stale);
    auto end = a.service->read_page(ar.ticket, av.stamp, {21, 1});
    CK_CHECK(end.state == FilesPaneSubmission::Accepted && a.reply().state == FilesPaneReplyState::OutOfRange);
    a.stopped();
    b.stopped();
}
CK_TEST(files_service_unread_results_and_wakeups_remain_bounded_until_taken) {
    Fixture f;
    f.start();
    auto one = f.service->navigate(address("one"));
    f.idle();
    CK_CHECK(f.trace->wakes == 1);
    for (int i = 0; i < 8; ++i) {
        f.service->navigate(address("two"));
        f.idle();
    }
    CK_CHECK(f.trace->wakes == 1);
    auto latest = f.reply();
    CK_CHECK(latest.state == FilesPaneReplyState::Ready && latest.request != one.ticket);
    CK_CHECK(!f.service->take());
    f.service->navigate(address("one"));
    auto next = f.reply();
    CK_CHECK(next.state == FilesPaneReplyState::Ready && f.trace->wakes == 2);
    f.stopped();
}
CK_TEST(files_service_stop_is_nonblocking_while_native_call_blocks_and_join_retires_resources) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    auto request = f.service->navigate(address("one"));
    f.trace->open_gate.wait();
    f.service->request_stop();
    CK_CHECK(f.service->running());
    CK_CHECK(f.service->navigate(address("two")).state == FilesPaneSubmission::Stopped);
    CK_CHECK(!f.service->cancel(request.ticket) && !f.service->take());
    f.trace->open_gate.release();
    f.stopped();
}
CK_TEST(files_service_exceptions_poison_worker_without_leaking_native_owners_or_retrying) {
    for (int fault : {1, 2}) {
        Fixture f;
        f.trace->open_fault = fault;
        f.start();
        f.service->navigate(address("one"));
        auto result = f.reply();
        CK_CHECK(result.state == FilesPaneReplyState::WorkerFault &&
                 result.worker_fault ==
                     (fault == 1 ? FilesPaneWorkerFault::Allocation : FilesPaneWorkerFault::Exception));
        CK_CHECK(!f.service->running() && f.service->navigate(address("two")).state == FilesPaneSubmission::Stopped);
        CK_CHECK(f.trace->opens == 1);
        f.stopped();
    }
}
CK_TEST(files_service_query_limits_and_workspace_refusals_remain_explicit) {
    Fixture f;
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->workspace_error =
            UnavailableObservation{ObservationState::Unsupported, {ErrorDomain::Adapter, 4, "workspace.refused"}};
    }
    f.start();
    f.service->navigate(address("one"));
    auto result = f.reply();
    CK_CHECK(result.state == FilesPaneReplyState::Failed && result.error->error.operation == "workspace.refused" &&
             f.trace->opens == 0);
    CK_CHECK(f.service->read_page(result.navigation, result.stamp, {0, 4097}).state ==
             FilesPaneSubmission::LimitExceeded);
    CK_CHECK(f.service->read_page({}, result.stamp, {}).state == FilesPaneSubmission::Invalid);
    f.stopped();
}
CK_TEST(files_service_oversized_diagnostic_is_a_resource_failure_not_truncated_native_success) {
    Fixture f;
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error =
            UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, std::string(100, 'x')}};
    }
    FilesPaneServiceLimits limits;
    limits.diagnostic_bytes = 99;
    f.start(limits);
    f.service->navigate(address("one"));
    auto result = f.reply();
    CK_CHECK(result.state == FilesPaneReplyState::LimitExceeded && !result.error && !result.page);
    f.stopped();
}
CK_TEST(files_service_bad_configuration_and_oversized_requests_fail_before_native_queries) {
    Fixture f;
    auto backend = f.backend();
    bool threw = false;
    try {
        FilesPaneService bad(backend, {0});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw && f.trace->opens == 0 && f.trace->workspaces == 0);
    FilesPaneServiceLimits limits;
    limits.path_bytes = 1;
    f.start(limits);
    CK_CHECK(f.service->navigate(address("one")).state == FilesPaneSubmission::LimitExceeded);
    auto moved = address("one");
    auto saved = std::move(moved);
    CK_CHECK(f.service->navigate(std::move(moved)).state == FilesPaneSubmission::Invalid);
    CK_CHECK(f.trace->opens == 0 && f.trace->workspaces == 0);
    f.stopped();
    (void)saved;
}
CK_TEST(files_service_first_page_limit_does_not_commit_an_unrenderable_navigation) {
    Fixture f;
    FilesPaneServiceLimits limits;
    limits.page_text_bytes = 32;
    f.start(limits);
    auto one = f.service->navigate(address("one"), {}, 1);
    auto first = f.reply();
    CK_CHECK(first.state == FilesPaneReplyState::Ready && first.page);
    auto two = f.service->navigate(address("two"), {}, 20);
    auto result = f.reply();
    CK_CHECK(result.request == two.ticket && result.state == FilesPaneReplyState::LimitExceeded && !result.page);
    CK_CHECK(*result.active_location == address("one") && first.stamp.current());
    auto read = f.service->read_page(two.ticket, result.stamp, {0, 1});
    CK_CHECK(read.state == FilesPaneSubmission::Accepted && f.reply().state == FilesPaneReplyState::Ready);
    f.stopped();
    (void)one;
}
CK_TEST(files_service_startup_allocation_failure_is_an_owner_free_terminal_reply) {
    Fixture f;
    f.trace->startup_fault = 1;
    f.start();
    auto request = f.service->navigate(address("one"));
    auto result = f.reply();
    CK_CHECK(result.request == request.ticket && result.state == FilesPaneReplyState::WorkerFault &&
             result.worker_fault == FilesPaneWorkerFault::Allocation);
    CK_CHECK(!result.page && !result.active_location && f.trace->opens == 0 && f.trace->workspaces == 0);
    f.stopped();
}
CK_TEST(files_service_failed_notification_poisoning_has_a_final_coalesced_fault_signal) {
    Fixture f;
    std::atomic<unsigned> attempts = 0;
    f.service = std::make_unique<FilesPaneService>(f.backend(), FilesPaneServiceLimits{}, [&] {
        if (++attempts == 1)
            throw std::runtime_error("notification failure");
        f.trace->notify();
    });
    auto request = f.service->navigate(address("one"));
    auto result = f.reply();
    CK_CHECK(result.request == request.ticket && result.state == FilesPaneReplyState::WorkerFault && attempts == 2);
    CK_CHECK(!f.service->running() && f.service->fault() == FilesPaneWorkerFault::Exception);
    f.stopped();
}
CK_TEST(files_service_production_native_backend_reads_real_source_without_exposing_native_owners) {
    Fixture f;
    ck::testing::ScratchDirectory source("files-service-source");
    std::filesystem::create_directory(source.path() / "folder");
    {
        std::ofstream file(source.path() / "plain", std::ios::binary);
        file << "payload";
        if (!file)
            throw std::runtime_error("native service fixture write");
    }
    f.service = std::make_unique<FilesPaneService>(
        native_files_pane_backend(take(NativeAbsolutePath::from_host(f.scratch.path()))), FilesPaneServiceLimits{},
        [t = f.trace] { t->notify(); });
    auto request = f.service->navigate(take(NativeAbsolutePath::from_host(source.path())));
    auto result = f.reply();
    CK_CHECK(result.request == request.ticket && result.state == FilesPaneReplyState::Ready && result.page &&
             result.page->rows().size() == 2);
    bool regular = false, directory = false;
    for (const auto &row : result.page->rows()) {
        if (row.display_name == "plain")
            regular = row.metadata.kind.value() && *row.metadata.kind.value() == ObjectKind::RegularFile &&
                      row.metadata.logical_size.value() && *row.metadata.logical_size.value() == 7;
        if (row.display_name == "folder")
            directory = row.metadata.kind.value() && *row.metadata.kind.value() == ObjectKind::Directory;
    }
    CK_CHECK(regular && directory);
    f.stopped();
    CK_CHECK(!result.page->stamp().current() && result.page->rows().size() == 2);
}
CK_TEST(files_service_short_wire_budget_never_publishes_an_incomplete_requested_viewport) {
    Fixture f;
    FilesPaneServiceLimits limits;
    limits.page_wire_bytes = 512;
    f.start(limits);
    auto first_request = f.service->navigate(address("one"), {}, 1);
    auto first = f.reply();
    CK_CHECK(first.state == FilesPaneReplyState::Ready && first.page && first.page->rows().size() == 1);
    auto next = f.service->navigate(address("two"), {}, 20);
    auto limited = f.reply();
    CK_CHECK(limited.request == next.ticket && limited.state == FilesPaneReplyState::LimitExceeded && !limited.page);
    CK_CHECK(*limited.active_location == address("one") && first.stamp.current());
    f.service->read_page(next.ticket, limited.stamp, {0, 20});
    auto read = f.reply();
    CK_CHECK(read.state == FilesPaneReplyState::LimitExceeded && !read.page);
    f.service->read_page(next.ticket, read.stamp, {0, 1});
    CK_CHECK(f.reply().state == FilesPaneReplyState::Ready);
    f.stopped();
    (void)first_request;
}
