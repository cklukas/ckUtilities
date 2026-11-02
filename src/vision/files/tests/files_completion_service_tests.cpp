// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_completion_service.hpp"
#include "files_location_text.hpp"
#include "native_index_file.hpp"
#include "scratch_directory.hpp"
#include "simulated_filesystem.hpp"
#include <cvision/testing/cktest.hpp>
#include <atomic>
#include <algorithm>
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
        // Sanitizer-instrumented worker startup can legitimately exceed the
        // normal test watchdog on a loaded host. Keep the ordinary timeout
        // unchanged; only an instrumented test binary gets the qualification
        // budget, and a missing gate still fails deterministically.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
        constexpr auto timeout = std::chrono::seconds(30);
#else
        constexpr auto timeout = std::chrono::seconds(5);
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
        constexpr auto timeout = std::chrono::seconds(30);
#else
        constexpr auto timeout = std::chrono::seconds(5);
#endif
        if (!cv.wait_for(lock, timeout, [&] { return entered; }))
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
    std::atomic<bool> block_child{false}, block_retire{false};
    std::atomic<int> child_fault{0};
    std::atomic<unsigned> child_opens{0}, cursors_closed{0}, entries_closed{0};
    std::atomic<unsigned> home_queries{0};
    std::atomic<bool> block_home{false};
    std::atomic<int> home_fault{0};
    Gate home_gate, enum_gate;
    std::atomic<bool> block_enum{false}, enum_error{false};
    std::atomic<unsigned> enum_reads{0};
    bool replace_child = false, long_child_name = false;
    Gate open_gate, read_gate, child_gate, retire_gate;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<UnavailableObservation> open_error, workspace_error, child_error;
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
NativeName child_name(std::string_view text);
std::unique_ptr<DirectoryCursor> traced(std::unique_ptr<DirectoryCursor>, std::shared_ptr<Trace>);
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
                fs_.put(root, child_name(std::string(name) + "-" + std::to_string(i)),
                        fs_.make_node(ObjectKind::Directory));
            fs_.put(root, child_name(".hidden"), fs_.make_node(ObjectKind::Directory));
            fs_.put(root, child_name("file"), fs_.make_node(ObjectKind::RegularFile));
            fs_.put(root, child_name("link"), fs_.make_node(ObjectKind::SymbolicLink));
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
        auto value = fs_.open_directory(path, policy);
        if (!value.value())
            return value;
        return Observation<std::unique_ptr<DirectoryCursor>>::available(traced(std::move(value).take_value(), trace_));
    }
    Observation<BasicMetadata> inspect_basic(const std::filesystem::path &path) const override {
        trace_->check();
        return fs_.inspect_basic(path);
    }
};
NativeName child_name(std::string_view text) {
#ifdef _WIN32
    std::u16string units(text.begin(), text.end());
    return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(std::string(text));
#endif
}
class TracedEntry final : public OpenedEntry {
    std::unique_ptr<OpenedEntry> entry_;
    std::shared_ptr<Trace> trace_;

  public:
    TracedEntry(std::unique_ptr<OpenedEntry> entry, std::shared_ptr<Trace> trace)
        : entry_(std::move(entry)), trace_(std::move(trace)) {
        trace_->check();
    }
    ~TracedEntry() override {
        trace_->check();
        ++trace_->entries_closed;
    }
    Observation<BasicMetadata> inspect_basic() const override {
        trace_->check();
        return entry_->inspect_basic();
    }
};
class TracedCursor final : public DirectoryCursor {
    std::unique_ptr<DirectoryCursor> cursor_;
    std::shared_ptr<Trace> trace_;

  public:
    TracedCursor(std::unique_ptr<DirectoryCursor> cursor, std::shared_ptr<Trace> trace)
        : DirectoryCursor(cursor->identity()), cursor_(std::move(cursor)), trace_(std::move(trace)) {
        trace_->check();
    }
    ~TracedCursor() override {
        trace_->check();
        if (trace_->block_retire.exchange(false))
            trace_->retire_gate.block();
        ++trace_->cursors_closed;
    }
    Observation<AdapterScope> source_scope() const override {
        trace_->check();
        return cursor_->source_scope();
    }
    Observation<BasicMetadata> inspect_basic() const override {
        trace_->check();
        return cursor_->inspect_basic();
    }
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName &name, EntryAccess access) const override {
        trace_->check();
        auto result = cursor_->open_child(name, access);
        if (!result.value())
            return result;
        return Observation<std::unique_ptr<OpenedEntry>>::available(
            std::make_unique<TracedEntry>(std::move(result).take_value(), trace_));
    }
    Observation<std::unique_ptr<DirectoryCursor>> open_directory_child(const NativeName &name) const override {
        trace_->check();
        ++trace_->child_opens;
        if (trace_->block_child.exchange(false))
            trace_->child_gate.block();
        if (trace_->child_fault == 1)
            throw std::bad_alloc();
        if (trace_->child_fault == 2)
            throw std::runtime_error("child navigation fault");
        {
            std::lock_guard lock(trace_->mutex);
            if (trace_->child_error) {
                const auto &e = *trace_->child_error;
                return Observation<std::unique_ptr<DirectoryCursor>>::unavailable(e.state, e.error);
            }
        }
        auto result = cursor_->open_directory_child(name);
        if (!result.value())
            return result;
        return Observation<std::unique_ptr<DirectoryCursor>>::available(
            std::make_unique<TracedCursor>(std::move(result).take_value(), trace_));
    }

  private:
    Observation<DirectoryStep> next_step() override {
        trace_->check();
        ++trace_->enum_reads;
        if (trace_->block_enum.exchange(false))
            trace_->enum_gate.block();
        if (trace_->enum_error.load())
            return Observation<DirectoryStep>::unavailable(ObservationState::Denied,
                                                           {ErrorDomain::Posix, 13, "enum.denied"});
        auto read = cursor_->read(1);
        if (read.error)
            return Observation<DirectoryStep>::unavailable(read.error->state, read.error->error);
        if (!read.names.empty())
            return Observation<DirectoryStep>::available(std::move(read.names.front()));
        if (read.state == DirectoryReadState::End)
            return Observation<DirectoryStep>::available(DirectoryEnd{});
        if (read.state == DirectoryReadState::More)
            return Observation<DirectoryStep>::available(DirectorySkip{});
        throw std::logic_error("unexpected traced cursor cancellation");
    }
};
std::unique_ptr<DirectoryCursor> traced(std::unique_ptr<DirectoryCursor> value, std::shared_ptr<Trace> trace) {
    return std::make_unique<TracedCursor>(std::move(value), std::move(trace));
}
class ChildSource final : public BasicFileSystem {
    std::shared_ptr<Trace> trace_;
    ck::testing::SimulatedFileSystem fs_;

  public:
    explicit ChildSource(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {
        trace_->check();
        for (const auto root_name : {"one", "two"}) {
            auto root = fs_.make_node(ObjectKind::Directory);
            fs_.bind(take(address(root_name).host_path()), root);
            auto folder = fs_.make_node(ObjectKind::Directory);
            auto inner = fs_.make_node(ObjectKind::Directory);
            fs_.put(inner, child_name("leaf"), fs_.make_node(ObjectKind::RegularFile));
            fs_.put(folder, child_name(trace_->long_child_name ? std::string(200, 'x') : "inner"), inner);
            fs_.put(root, child_name("folder"), folder);
            fs_.put(root, child_name("file"), fs_.make_node(ObjectKind::RegularFile));
            fs_.put(root, child_name("link"), fs_.make_node(ObjectKind::SymbolicLink));
            if (trace_->replace_child && root_name == std::string_view("one")) {
                auto replacement = fs_.make_node(ObjectKind::Directory);
                fs_.enqueue(
                    ck::testing::SimulatedFileSystem::Operation::OpenDirectoryChild,
                    {.effect = [this, root, replacement] { fs_.put(root, child_name("folder"), replacement); }});
            }
        }
    }
    ~ChildSource() override {
        trace_->check();
        ++trace_->fs_closed;
    }
    Observation<std::unique_ptr<DirectoryCursor>> open_directory(const std::filesystem::path &path,
                                                                 DirectoryLinkPolicy policy) const override {
        trace_->check();
        ++trace_->opens;
        auto result = fs_.open_directory(path, policy);
        if (!result.value())
            return result;
        return Observation<std::unique_ptr<DirectoryCursor>>::available(
            std::make_unique<TracedCursor>(std::move(result).take_value(), trace_));
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
    ck::testing::ScratchDirectory scratch{"completion-service", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT};
    std::shared_ptr<Trace> trace = std::make_shared<Trace>();
    std::unique_ptr<FilesCompletionService> service;
    Fixture() {
#ifndef _WIN32
        std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
    }
    ~Fixture() {
        trace->open_gate.release();
        trace->read_gate.release();
        trace->child_gate.release();
        trace->retire_gate.release();
        trace->home_gate.release();
        trace->enum_gate.release();
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
#ifdef _WIN32
        b.storage_model = StorageModel::WindowsHandleAttributes;
#else
        b.storage_model = StorageModel::LinuxStatx;
#endif
        b.home = [t = trace](NativePathLimits limits, std::stop_token stop) {
            t->check();
            ++t->home_queries;
            if (t->block_home.exchange(false))
                t->home_gate.block();
            if (t->home_fault == 1)
                return Observation<NativeAbsolutePath>::unavailable(ObservationState::Denied,
                                                                    {ErrorDomain::Posix, 13, "injected.home"});
            if (t->home_fault == 2)
                throw std::bad_alloc();
            if (t->home_fault == 3)
                throw std::runtime_error("home discovery fault");
            if (t->home_fault == 4) {
                auto value = address("two");
                const auto moved = std::move(value);
                (void)moved;
                return Observation<NativeAbsolutePath>::available(std::move(value));
            }
            (void)limits;
            (void)stop; // Deliberately untrusted resolver, checked again by the service.
            return Observation<NativeAbsolutePath>::available(
                address(t->home_fault == 5 ? "far-too-long-home-address" : "two"));
        };
        return b;
    }
    void start(FilesCompletionServiceLimits limits = {}) {
        service = std::make_unique<FilesCompletionService>(backend(), limits, [t = trace] { t->notify(); });
    }
    void start_children(FilesCompletionServiceLimits limits = {}) {
        auto b = backend();
        b.filesystem = [t = trace] { return std::make_unique<ChildSource>(t); };
#ifdef _WIN32
        b.storage_model = StorageModel::WindowsHandleAttributes;
#endif
        service = std::make_unique<FilesCompletionService>(std::move(b), limits, [t = trace] { t->notify(); });
    }
    FilesCompletionReply reply() {
        std::optional<FilesCompletionReply> value;
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

FilesLocationCompletionQuery query(const char *parent = "one", std::string_view prefix = "one-") {
    auto text = take(format_files_exact_location(FilesPaneLocation(address(parent))));
    text.pop_back();
    text += ", \"";
    text += prefix;
    return take(parse_files_location_completion(text, FilesLocationTextSyntax::ExactNames, address(parent).encoding()));
}
void accepted(const FilesCompletionSubmitted &r) {
    CK_CHECK(r.state == FilesCompletionSubmission::Accepted && r.ticket.valid());
}
void ready(const FilesCompletionReply &r, const FilesCompletionSubmitted &request, std::uint64_t revision) {
    CK_CHECK(r.request == request.ticket && r.revision == revision && r.state == FilesCompletionReplyState::Ready &&
             r.page);
}
} // namespace
CK_TEST(completion_service_is_lazy_and_keeps_native_work_and_retirement_on_one_worker) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    CK_CHECK(f.trace->opens == 0 && f.trace->workspaces == 0);
    auto submitted = f.service->request(query(), 1);
    accepted(submitted);
    f.trace->open_gate.wait();
    CK_CHECK(f.service->working() && !f.service->take());
    f.trace->open_gate.release();
    auto reply = f.reply();
    ready(reply, submitted, 1);
    CK_CHECK(reply.page->entries.size() == 20 && reply.page->complete());
    f.stopped();
    CK_CHECK(f.trace->fs_closed == 1 && f.trace->factory_closed == 1 && f.trace->files_closed > 0);
    CK_CHECK(f.service->request(query(), 2).state == FilesCompletionSubmission::Stopped);
}
CK_TEST(completion_service_same_parent_prefix_edits_share_blocked_open_and_keep_latest_ticket_revision) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    auto old = f.service->request(query(), 1);
    accepted(old);
    f.trace->open_gate.wait();
    FilesCompletionSubmitted latest = old;
    for (std::uint64_t revision = 2; revision <= 100; ++revision)
        latest = f.service->request(query("one", "one-1"), revision);
    accepted(latest);
    CK_CHECK(!f.service->cancel(old.ticket));
    f.trace->open_gate.release();
    auto reply = f.reply();
    ready(reply, latest, 100);
    CK_CHECK(f.trace->opens == 1 && reply.page->entries.size() == 11 && !f.service->take());
    auto restarted = f.service->request(query(), 1);
    accepted(restarted);
    reply = f.reply();
    ready(reply, restarted, 1);
    CK_CHECK(restarted.ticket != old.ticket && f.trace->opens == 1);
    f.stopped();
}
CK_TEST(completion_service_same_parent_edits_share_partial_native_enumeration_and_reuse_completed_index) {
    Fixture f;
    f.trace->block_enum = true;
    f.start();
    accepted(f.service->request(query(), 1));
    f.trace->enum_gate.wait();
    accepted(f.service->request(query("one", "one-2"), 2));
    const auto latest = f.service->request(query("one", "one-3"), 3);
    accepted(latest);
    f.trace->enum_gate.release();
    auto reply = f.reply();
    ready(reply, latest, 3);
    CK_CHECK(reply.page->entries.size() == 1 && f.trace->opens == 1);
    const auto read_count = f.trace->enum_reads.load();
    const auto cached = f.service->request(query("one", "one-1"), 4);
    reply = f.reply();
    ready(reply, cached, 4);
    CK_CHECK(f.trace->enum_reads == read_count && f.trace->opens == 1 && f.trace->workspaces == 1);
    f.stopped();
}
CK_TEST(completion_service_superseding_match_preserves_index_instead_of_reenumerating) {
    Fixture f;
    f.start();
    accepted(f.service->request(query(), 1));
    (void)f.reply();
    const auto enumerated = f.trace->enum_reads.load();
    f.trace->block_read = true;
    auto old = f.service->request(query("one", "one-0"), 2);
    accepted(old);
    f.trace->read_gate.wait();
    auto latest = f.service->request(query("one", "one-1"), 3);
    accepted(latest);
    f.trace->read_gate.release();
    auto reply = f.reply();
    ready(reply, latest, 3);
    CK_CHECK(reply.page->entries.size() == 11 && f.trace->opens == 1 && f.trace->enum_reads == enumerated);
    f.stopped();
}
CK_TEST(completion_service_different_parent_supersedes_blocked_discovery_without_old_delivery) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    accepted(f.service->request(query(), 1));
    f.trace->open_gate.wait();
    auto latest = f.service->request(query("two", "two-"), 2);
    accepted(latest);
    f.trace->open_gate.release();
    auto reply = f.reply();
    ready(reply, latest, 2);
    CK_CHECK(reply.page->entries.size() == 20 && f.trace->opens == 2);
    CK_CHECK(!f.service->take());
    f.stopped();
}
CK_TEST(completion_service_cancelled_discovery_has_no_partial_payload_and_can_restart) {
    Fixture f;
    f.trace->block_enum = true;
    f.start();
    auto current = f.service->request(query(), 7);
    accepted(current);
    f.trace->enum_gate.wait();
    CK_CHECK(f.service->cancel(current.ticket));
    f.trace->enum_gate.release();
    auto reply = f.reply();
    CK_CHECK(reply.request == current.ticket && reply.revision == 7 &&
             reply.state == FilesCompletionReplyState::Cancelled && !reply.page);
    auto next = f.service->request(query(), 8);
    reply = f.reply();
    ready(reply, next, 8);
    CK_CHECK(f.trace->opens == 2);
    f.stopped();
}
CK_TEST(completion_service_native_enumeration_failure_wins_over_simultaneous_cancel) {
    Fixture f;
    f.trace->block_enum = true;
    f.start();
    auto current = f.service->request(query(), 9);
    accepted(current);
    f.trace->enum_gate.wait();
    f.trace->enum_error = true;
    CK_CHECK(f.service->cancel(current.ticket));
    f.trace->enum_gate.release();
    auto reply = f.reply();
    CK_CHECK(reply.request == current.ticket && reply.revision == 9 &&
             reply.state == FilesCompletionReplyState::Failed && !reply.page &&
             reply.error->error.operation == "enum.denied");
    f.stopped();
}
CK_TEST(completion_service_cancels_unread_success_but_does_not_erase_unread_failure) {
    Fixture f;
    f.start();
    auto current = f.service->request(query(), 1);
    accepted(current);
    f.idle();
    CK_CHECK(f.service->cancel(current.ticket));
    auto reply = f.reply();
    CK_CHECK(reply.state == FilesCompletionReplyState::Cancelled && !reply.page && !f.service->cancel(current.ticket));
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error = UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "denied"}};
    }
    auto failed = f.service->request(query("two", "two-"), 2);
    accepted(failed);
    f.idle();
    CK_CHECK(!f.service->cancel(failed.ticket));
    reply = f.reply();
    CK_CHECK(reply.error->error.operation == "denied" && reply.state == FilesCompletionReplyState::Failed);
    f.stopped();
}
CK_TEST(completion_service_failed_other_parent_preserves_previous_cache_without_relabeling_error) {
    Fixture f;
    f.start();
    accepted(f.service->request(query(), 1));
    auto first = f.reply();
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error =
            UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "other.denied"}};
    }
    const auto failed = f.service->request(query("two", "two-"), 2);
    auto error_reply = f.reply();
    CK_CHECK(error_reply.request == failed.ticket && error_reply.revision == 2 &&
             error_reply.error->error.operation == "other.denied");
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error.reset();
    }
    const auto returned = f.service->request(query(), 3);
    auto reply = f.reply();
    ready(reply, returned, 3);
    CK_CHECK(f.trace->opens == 2 && reply.page->index_scope == first.page->index_scope);
    f.stopped();
}
CK_TEST(completion_service_refresh_rotates_index_and_old_continuations_are_stale_without_io) {
    Fixture f;
    FilesCompletionServiceLimits limits;
    limits.matches.candidates = 2;
    f.start(limits);
    accepted(f.service->request(query(), 1));
    auto first = f.reply();
    CK_CHECK(first.page->next);
    auto refreshed = f.service->request(query(), 2, {}, {}, true);
    auto fresh = f.reply();
    ready(fresh, refreshed, 2);
    CK_CHECK(f.trace->opens == 2 && first.page->index_scope != fresh.page->index_scope);
    const auto reads = f.trace->reads.load();
    auto stale = f.service->request(query(), 3, {}, first.page->next);
    auto reply = f.reply();
    CK_CHECK(reply.request == stale.ticket && reply.state == FilesCompletionReplyState::Stale && !reply.page &&
             f.trace->reads == reads && f.trace->opens == 2);
    CK_CHECK(f.service->request(query(), 4, {}, fresh.page->next, true).state == FilesCompletionSubmission::Invalid);
    f.stopped();
}
CK_TEST(completion_service_failed_refresh_invalidates_target_cache_and_never_silently_reuses_it) {
    Fixture f;
    FilesCompletionServiceLimits limits;
    limits.matches.candidates = 2;
    f.start(limits);
    accepted(f.service->request(query(), 1));
    auto first = f.reply();
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error =
            UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "refresh.denied"}};
    }
    accepted(f.service->request(query(), 2, {}, {}, true));
    CK_CHECK(f.reply().state == FilesCompletionReplyState::Failed);
    accepted(f.service->request(query(), 3, {}, first.page->next));
    CK_CHECK(f.reply().state == FilesCompletionReplyState::Stale && f.trace->opens == 2);
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->open_error.reset();
    }
    auto fresh = f.service->request(query(), 4);
    auto reply = f.reply();
    ready(reply, fresh, 4);
    CK_CHECK(f.trace->opens == 3 && reply.page->index_scope != first.page->index_scope);
    f.stopped();
}
CK_TEST(completion_service_pagination_reuses_discovery_with_exact_ticket_and_revision_per_page) {
    Fixture f;
    FilesCompletionServiceLimits limits;
    limits.matches.candidates = 3;
    f.start(limits);
    std::optional<FilesCompletionCursor> after;
    std::uint64_t revision = 0;
    std::vector<NativeName> names;
    do {
        const auto submitted = f.service->request(query(), ++revision, {}, after);
        accepted(submitted);
        auto reply = f.reply();
        ready(reply, submitted, revision);
        CK_CHECK(reply.page->has_previous == (revision > 1));
        for (const auto &entry : reply.page->entries)
            names.push_back(entry.name);
        after = reply.page->next;
    } while (after);
    CK_CHECK(names.size() == 20 && revision == 7 && f.trace->opens == 1);
    for (std::size_t i = 1; i < names.size(); ++i)
        CK_CHECK(compare_native_names(names[i - 1], names[i]) < 0);
    f.stopped();
}
CK_TEST(completion_service_replays_literal_parent_components_through_held_cursors) {
    Fixture f;
    f.start_children();
    auto route = take(FilesPaneLocation(address("one")).descend(child_name("folder"), {}, 10));
    auto text = take(format_files_exact_location(route));
    text.pop_back();
    text += ", \"inn";
    auto q =
        take(parse_files_location_completion(text, FilesLocationTextSyntax::ExactNames, address("one").encoding()));
    auto submitted = f.service->request(q, 1);
    auto reply = f.reply();
    ready(reply, submitted, 1);
    CK_CHECK(reply.page->entries.size() == 1 && f.trace->opens == 1 && f.trace->child_opens == 1);
    auto location = take(parse_files_location_text(reply.page->entries[0].text, FilesLocationTextSyntax::ExactNames,
                                                   address("one").encoding()));
    CK_CHECK(location.components().size() == 2 && location.components()[0] == child_name("folder") &&
             location.components()[1] == child_name("inner"));
    submitted = f.service->request(q, 2);
    reply = f.reply();
    ready(reply, submitted, 2);
    CK_CHECK(f.trace->child_opens == 1);
    f.stopped();
}
CK_TEST(completion_service_submission_limits_and_invalid_revisions_are_checked_before_native_work) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        FilesCompletionServiceLimits limits;
        if (fault == 0)
            limits.query.path.max_native_bytes = 1;
        if (fault == 1)
            limits.query.text_bytes = 1;
        if (fault == 2)
            limits.query.components = 1;
        f.start(limits);
        auto q = query();
        if (fault == 2) {
            auto route = take(FilesPaneLocation(address("one")).descend(child_name("folder"), {}, 10));
            auto text = take(format_files_exact_location(route));
            text.pop_back();
            text += ", \"a";
            q = take(
                parse_files_location_completion(text, FilesLocationTextSyntax::ExactNames, address("one").encoding()));
        }
        CK_CHECK(f.service->request(q, 1).state == FilesCompletionSubmission::LimitExceeded && f.trace->opens == 0 &&
                 f.trace->workspaces == 0);
        CK_CHECK(f.service->request(query(), 0).state == FilesCompletionSubmission::Invalid);
        f.stopped();
    }
    Fixture f;
    f.start();
    auto q = query();
    const auto retained = std::move(q);
    (void)retained;
    CK_CHECK(f.service->request(std::move(q), 1).state == FilesCompletionSubmission::Invalid);
    f.stopped();
}
CK_TEST(completion_service_workspace_and_cache_read_errors_are_explicit_and_retry_is_worker_owned) {
    Fixture f;
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->workspace_error =
            UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "workspace.denied"}};
    }
    f.start();
    accepted(f.service->request(query(), 1));
    auto reply = f.reply();
    CK_CHECK(reply.state == FilesCompletionReplyState::Failed && reply.error->error.operation == "workspace.denied" &&
             f.trace->opens == 0);
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->workspace_error.reset();
    }
    auto submitted = f.service->request(query(), 2);
    reply = f.reply();
    ready(reply, submitted, 2);
    f.trace->read_error = true;
    accepted(f.service->request(query(), 3));
    reply = f.reply();
    CK_CHECK(reply.state == FilesCompletionReplyState::Failed && reply.error->error.operation == "cache.read.denied");
    submitted = f.service->request(query(), 4);
    reply = f.reply();
    ready(reply, submitted, 4);
    CK_CHECK(f.trace->opens == 2 && f.service->fault() == FilesPaneWorkerFault::None);
    f.stopped();
}
CK_TEST(completion_service_terminal_allocation_and_exception_faults_do_not_retry_or_publish_prefixes) {
    for (int fault = 1; fault <= 2; ++fault) {
        Fixture f;
        f.trace->startup_fault = fault;
        f.start();
        auto submitted = f.service->request(query(), 55);
        accepted(submitted);
        auto reply = f.reply();
        CK_CHECK(reply.request == submitted.ticket && reply.revision == 55 &&
                 reply.state == FilesCompletionReplyState::WorkerFault && !reply.page);
        CK_CHECK(f.service->fault() ==
                 (fault == 1 ? FilesPaneWorkerFault::Allocation : FilesPaneWorkerFault::Exception));
        CK_CHECK(f.service->request(query(), 56).state == FilesCompletionSubmission::Stopped &&
                 !f.service->cancel(submitted.ticket));
        f.stopped();
    }
}
CK_TEST(completion_service_stop_is_nonblocking_while_os_work_is_blocked_and_join_retires_every_owner) {
    Fixture f;
    f.trace->block_open = true;
    f.start();
    accepted(f.service->request(query(), 1));
    f.trace->open_gate.wait();
    f.service->request_stop();
    CK_CHECK(f.service->running() && f.service->working() && f.trace->fs_closed == 0);
    CK_CHECK(f.service->request(query(), 2).state == FilesCompletionSubmission::Stopped);
    f.trace->open_gate.release();
    f.stopped();
    CK_CHECK(!f.service->take() && f.trace->wakes == 0 && f.trace->fs_closed == 1 && f.trace->factory_closed == 1);
}
CK_TEST(completion_service_cache_retirement_blocks_only_worker_and_can_coalesce_new_prefixes) {
    Fixture f;
    f.start();
    accepted(f.service->request(query(), 1));
    (void)f.reply();
    f.trace->block_retire = true;
    accepted(f.service->request(query(), 2, {}, {}, true));
    f.trace->retire_gate.wait();
    auto submitted = f.service->request(query("one", "one-1"), 3);
    accepted(submitted);
    CK_CHECK(f.service->working());
    f.trace->retire_gate.release();
    auto reply = f.reply();
    ready(reply, submitted, 3);
    CK_CHECK(reply.page->entries.size() == 11 && f.trace->opens == 2);
    f.stopped();
}
CK_TEST(completion_service_notifications_coalesce_until_take_and_callback_failure_poisoning_is_observable) {
    {
        Fixture f;
        f.start();
        accepted(f.service->request(query(), 1));
        f.idle();
        auto latest = f.service->request(query("one", "one-1"), 2);
        f.idle();
        CK_CHECK(f.trace->wakes == 1);
        auto reply = f.reply();
        ready(reply, latest, 2);
        latest = f.service->request(query(), 3);
        reply = f.reply();
        ready(reply, latest, 3);
        CK_CHECK(f.trace->wakes == 2);
        f.stopped();
    }
    Fixture f;
    f.service = std::make_unique<FilesCompletionService>(f.backend(), FilesCompletionServiceLimits{}, [t = f.trace] {
        t->notify();
        throw std::runtime_error("wake fault");
    });
    auto submitted = f.service->request(query(), 4);
    accepted(submitted);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.service->running()) {
        if (std::chrono::steady_clock::now() > until)
            throw std::runtime_error("worker fault wait");
        std::this_thread::yield();
    }
    auto reply = f.service->take();
    CK_CHECK(reply && reply->request == submitted.ticket && reply->state == FilesCompletionReplyState::WorkerFault &&
             f.service->fault() == FilesPaneWorkerFault::Exception);
    f.stopped();
}
CK_TEST(completion_service_resource_limits_and_oversized_diagnostics_never_become_success) {
    for (int fault = 0; fault < 3; ++fault) {
        Fixture f;
        FilesCompletionServiceLimits limits;
        if (fault == 0)
            limits.directory_rows = 1;
        if (fault == 1)
            limits.cache_bytes = 16;
        if (fault == 2) {
            limits.diagnostic_bytes = 16;
            std::lock_guard lock(f.trace->mutex);
            f.trace->open_error =
                UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, std::string(1024, 'x')}};
        }
        f.start(limits);
        accepted(f.service->request(query(), 1));
        auto reply = f.reply();
        CK_CHECK(reply.state == FilesCompletionReplyState::LimitExceeded && !reply.page);
        if (fault == 2)
            CK_CHECK(!reply.error);
        f.stopped();
    }
}
CK_TEST(completion_service_native_backend_discovers_real_directories_and_keeps_pages_owner_free) {
    Fixture f;
    const auto source = f.scratch.path() / "source";
    std::filesystem::create_directory(source);
    std::filesystem::create_directory(source / "alpha");
    std::filesystem::create_directory(source / "alpine");
    std::ofstream(source / "file") << "fixture";
    f.service = std::make_unique<FilesCompletionService>(
        native_files_pane_backend(take(NativeAbsolutePath::from_host(f.scratch.path()))),
        FilesCompletionServiceLimits{}, [t = f.trace] { t->notify(); });
    auto path = take(NativeAbsolutePath::from_host(source));
    auto text = take(format_files_exact_location(FilesPaneLocation(path)));
    text.pop_back();
    text += ", \"al";
    auto q = take(parse_files_location_completion(text, FilesLocationTextSyntax::ExactNames, path.encoding()));
    auto submitted = f.service->request(q, 42);
    auto reply = f.reply();
    ready(reply, submitted, 42);
    CK_CHECK(reply.page->entries.size() == 2);
    f.service->join();
    CK_CHECK(reply.page->entries[0].text.size() > 0);
    std::filesystem::remove_all(source);
    f.stopped();
}
CK_TEST(completion_service_invalid_configuration_is_rejected_before_thread_or_native_creation) {
    for (int fault = 0; fault < 30; ++fault) {
        Fixture f;
        auto b = f.backend();
        FilesCompletionServiceLimits l;
        if (fault == 0)
            l.query.path.max_native_bytes = 0;
        if (fault == 1)
            l.query.path.max_native_bytes = NativePathLimits::hard_native_bytes + 1;
        if (fault == 2)
            l.query.components = 0;
        if (fault == 3)
            l.query.components = FilesPaneLocation::hard_components + 1;
        if (fault == 4)
            l.query.text_bytes = 0;
        if (fault == 5)
            l.query.text_bytes = 32769;
        if (fault == 6)
            l.matches.scan.rows = 0;
        if (fault == 7)
            l.matches.scan.rows = DirectoryIndexScanLimits::hard_rows + 1;
        if (fault == 8)
            l.matches.scan.wire_bytes = 0;
        if (fault == 9)
            l.matches.scan.wire_bytes = DirectoryIndexScanLimits::hard_wire_bytes + 1;
        if (fault == 10)
            l.matches.candidates = 0;
        if (fault == 11)
            l.matches.candidates = 65;
        if (fault == 12)
            l.matches.text_bytes = 0;
        if (fault == 13)
            l.matches.text_bytes = 1024 * 1024 + 1;
        if (fault == 14)
            l.matches.native_name_bytes = 0;
        if (fault == 15)
            l.matches.native_name_bytes = 1024 * 1024 + 1;
        if (fault == 16)
            l.matches.candidate_text_bytes = 0;
        if (fault == 17)
            l.matches.candidate_text_bytes = 32769;
        if (fault == 18)
            l.directory_rows = 0;
        if (fault == 19)
            l.directory_rows = 1'000'001;
        if (fault == 20)
            l.cache_bytes = 0;
        if (fault == 21)
            l.cache_bytes = 8ULL * 1024 * 1024 * 1024 + 1;
        if (fault == 22)
            l.cache_files = 0;
        if (fault == 23)
            l.cache_files = 257;
        if (fault == 24)
            l.diagnostic_bytes = 0;
        if (fault == 25)
            l.diagnostic_bytes = 1024 * 1024 + 1;
        if (fault == 26)
            b.filesystem = {};
        if (fault == 27)
            b.workspace = {};
        if (fault == 28)
            b.clock.reset();
        if (fault == 29)
            b.storage_model = static_cast<StorageModel>(-1);
        bool threw = false;
        try {
            FilesCompletionService invalid(std::move(b), l);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CK_CHECK(threw && f.trace->opens == 0 && f.trace->workspaces == 0 && f.trace->worker == std::thread::id{});
    }
}
CK_TEST(completion_service_null_backend_owners_are_terminal_faults_with_no_partial_reply) {
    for (int fault = 0; fault < 2; ++fault) {
        Fixture f;
        auto b = f.backend();
        if (fault == 0)
            b.filesystem = [t = f.trace]() -> std::unique_ptr<BasicFileSystem> {
                t->check();
                return {};
            };
        if (fault == 1)
            b.workspace = [t = f.trace] {
                t->check();
                return Observation<std::shared_ptr<IndexFileFactory>>::available({});
            };
        f.service = std::make_unique<FilesCompletionService>(std::move(b), FilesCompletionServiceLimits{},
                                                             [t = f.trace] { t->notify(); });
        auto request = f.service->request(query(), 1);
        auto reply = f.reply();
        CK_CHECK(reply.request == request.ticket && reply.state == FilesCompletionReplyState::WorkerFault &&
                 reply.fault == FilesPaneWorkerFault::Exception && !reply.page);
        f.stopped();
    }
}
CK_TEST(completion_service_hidden_default_and_override_and_proposal_diagnostic_limit_are_explicit) {
    {
        Fixture f;
        FilesCompletionServiceLimits limits;
        limits.matches.show_hidden = false;
        f.start(limits);
        accepted(f.service->request(query("one", ""), 1));
        auto filtered = f.reply();
#ifdef _WIN32
        CK_CHECK(filtered.page->unknown_hidden > 0 && !filtered.page->complete());
#else
        CK_CHECK(filtered.page->entries.size() == 20 && filtered.page->complete());
#endif
        accepted(f.service->request(query("one", ""), 2, true));
        auto all = f.reply();
        CK_CHECK(all.page->entries.size() == 21 && all.page->unknown_hidden == 0 && all.page->complete() &&
                 f.trace->opens == 1);
        f.stopped();
    }
    Fixture f;
    FilesCompletionServiceLimits limits;
    limits.query.text_bytes = take(format_files_exact_location(FilesPaneLocation(address("one")))).size() + 2;
    limits.diagnostic_bytes = 1;
    f.start(limits);
    accepted(f.service->request(query("one", ""), 1));
    auto reply = f.reply();
    CK_CHECK(reply.state == FilesCompletionReplyState::LimitExceeded && !reply.page && !reply.error);
    f.stopped();
}
CK_TEST(completion_service_discovery_failure_is_not_relabelled_as_a_later_edit) {
    Fixture f;
    auto b = f.backend();
    auto workspace = b.workspace;
    {
        std::lock_guard lock(f.trace->mutex);
        f.trace->workspace_error =
            UnavailableObservation{ObservationState::Denied, {ErrorDomain::Posix, 13, "workspace.denied"}};
    }
    auto first = std::make_shared<std::atomic<bool>>(true);
    b.workspace = [t = f.trace, workspace, first] {
        if (first->exchange(false))
            t->home_gate.block();
        return workspace();
    };
    f.service = std::make_unique<FilesCompletionService>(std::move(b), FilesCompletionServiceLimits{},
                                                         [t = f.trace] { t->notify(); });
    accepted(f.service->request(query(), 1));
    f.trace->home_gate.wait();
    auto latest = f.service->request(query("one", "one-1"), 2);
    accepted(latest);
    f.trace->home_gate.release();
    auto reply = f.reply();
    CK_CHECK(reply.request == latest.ticket && reply.revision == 2 &&
             reply.error->error.operation == "workspace.denied" && f.trace->workspaces == 2 && !reply.page);
    f.stopped();
}
CK_TEST(completion_service_cache_read_failure_wins_over_cancellation_and_invalidates_only_its_cache) {
    Fixture f;
    f.start();
    accepted(f.service->request(query(), 1));
    (void)f.reply();
    f.trace->block_read = true;
    f.trace->read_error = true;
    auto current = f.service->request(query(), 2);
    accepted(current);
    f.trace->read_gate.wait();
    CK_CHECK(f.service->cancel(current.ticket));
    f.trace->read_gate.release();
    auto reply = f.reply();
    CK_CHECK(reply.request == current.ticket && reply.revision == 2 &&
             reply.state == FilesCompletionReplyState::Failed && reply.error->error.operation == "cache.read.denied" &&
             !reply.page);
    current = f.service->request(query(), 3);
    reply = f.reply();
    ready(reply, current, 3);
    CK_CHECK(f.trace->opens == 2);
    f.stopped();
}
