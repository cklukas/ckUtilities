// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_pane_service.hpp"
#include "native_index_file.hpp"
#include "native_home.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
namespace ck::vision {
static_assert(std::is_nothrow_move_constructible_v<FilesPaneReply> &&
              std::is_nothrow_move_assignable_v<FilesPaneReply>);
namespace {
using namespace ck::files;
struct ChildNavigation {
    FilesPaneTicket navigation;
    PaneStateStamp stamp;
    PaneRowId row;
};
struct HomeNavigation {};
struct Navigate {
    FilesPaneTicket ticket;
    std::variant<FilesPaneLocation, ChildNavigation, HomeNavigation> destination;
    PaneView view;
    std::size_t rows;
};
struct Read {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    FilesPaneReadRange range;
};
struct Change {
    Read request;
    FilesPaneChange command;
};
struct Preview {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    PaneRowId row;
    FilesPanePreviewLimits limits;
};
struct CreateDirectory {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    NativeName name;
    FilesPaneReadRange range;
};
struct CreateFile {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    NativeName name;
    FilesPaneReadRange range;
};
struct CreateHardLink {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    PaneRowId row;
    NativeName name;
    FilesPaneReadRange range;
};
struct CreateSymbolicLink {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    SymbolicLinkTarget target;
    NativeName name;
    FilesPaneReadRange range;
};
struct RenameChild {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    PaneRowId row;
    NativeName name;
    FilesPaneReadRange range;
};
struct DeleteChild {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    PaneRowId row;
    FilesPaneReadRange range;
};
struct TrashChild {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    PaneRowId row;
    FilesPaneReadRange range;
};
struct OperationSources {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    IndexedSourceLimits limits;
    FilesPaneOperationSourceHandoff handoff;
};
struct OperationDestination {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    FilesPaneOperationDestinationHandoff handoff;
};
using Operation = std::variant<OperationSources, OperationDestination, CreateDirectory, CreateFile, CreateHardLink, CreateSymbolicLink,
                               RenameChild, DeleteChild, TrashChild>;
bool valid(FilesPaneServiceLimits l) {
    return l.path_bytes && l.path_bytes <= NativePathLimits::hard_native_bytes && l.page_rows && l.page_rows <= 4096 &&
           l.page_text_bytes && l.page_text_bytes <= 16 * 1024 * 1024 && l.page_wire_bytes >= 16 &&
           l.page_wire_bytes <= DirectoryIndexScanLimits::hard_wire_bytes && l.diagnostic_bytes &&
           l.diagnostic_bytes <= 1024 * 1024 && l.directory_rows && l.directory_rows <= 1'000'000 && l.cache_bytes &&
           l.cache_bytes <= 8ULL * 1024 * 1024 * 1024 && l.cache_files && l.cache_files <= 256 &&
           l.location_components && l.location_components <= FilesPaneLocation::hard_components;
}
UnavailableObservation operation_error(AdapterError code, const char *operation,
                                       ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
void cancel_operation(Operation &operation) noexcept {
    try {
        std::visit(
            [](auto &request) {
                using Request = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Request, OperationSources>) {
                    request.handoff(Observation<FilesPaneOperationSources>::unavailable(
                        ObservationState::Unknown,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead),
                         "pane.operation_sources.stopped"}));
                } else if constexpr (std::is_same_v<Request, OperationDestination>) {
                    request.handoff(Observation<FilesPaneOperationDestination>::unavailable(
                        ObservationState::Unknown,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead),
                         "pane.operation_destination.stopped"}));
                }
            },
            operation);
    } catch (...) {
        // request_stop() is noexcept; the worker remains responsible for
        // reporting callback failures when it is actively delivering work.
    }
}
} // namespace
struct FilesPaneService::State {
    FilesPaneBackend backend;
    const FilesPaneServiceLimits limits;
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Navigate> navigation;
    std::optional<Read> read;
    std::optional<Change> change;
    std::optional<Preview> preview;
    std::optional<Operation> operation;
    std::optional<FilesPaneReply> reply;
    FilesPaneTicket latest_navigation, latest_request, cancelled;
    std::optional<std::stop_source> active_stop;
    bool navigation_busy = false, change_busy = false, busy = false, stopping = false, signalled = false;
    std::uint64_t next = 1;
    std::atomic<bool> alive{true};
    std::atomic<FilesPaneWorkerFault> worker_fault{FilesPaneWorkerFault::None};
    std::jthread worker;
    State(FilesPaneBackend b, FilesPaneServiceLimits l, std::function<void()> w)
        : backend(std::move(b)), limits(l), wake(std::move(w)) {}
    FilesPaneSubmitted submit_navigation(std::variant<FilesPaneLocation, ChildNavigation, HomeNavigation> destination,
                                         PaneView view, std::size_t first_page_rows) {
        std::optional<std::stop_source> stop;
        FilesPaneTicket request;
        {
            std::lock_guard lock(mutex);
            if (stopping || !alive.load())
                return {FilesPaneSubmission::Stopped, {}};
            if (operation)
                return {FilesPaneSubmission::Busy, {}};
            if (!next)
                return {FilesPaneSubmission::Exhausted, {}};
            request = FilesPaneService::ticket(next++);
            navigation.emplace(Navigate{request, std::move(destination), std::move(view), first_page_rows});
            read.reset();
            change.reset();
            preview.reset();
            operation.reset();
            change_busy = false;
            reply.reset();
            cancelled = {};
            latest_navigation = latest_request = request;
            navigation_busy = true;
            stop = active_stop;
        }
        if (stop)
            stop->request_stop();
        condition.notify_one();
        return {FilesPaneSubmission::Accepted, request};
    }
    void notify(bool needed) {
        if (needed && wake)
            wake();
    }
    bool signal() noexcept { return !std::exchange(signalled, true); }
    void emit(FilesPaneReply result, bool is_navigation, bool is_change = false) {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            busy = false;
            if (is_change)
                change_busy = false;
            active_stop.reset();
            if (stopping || result.request != latest_request)
                return;
            if (is_navigation)
                navigation_busy = false;
            // Never truncate an adapter diagnostic into apparent native success.
            if (result.error && result.error->error.operation.size() > limits.diagnostic_bytes) {
                result.error.reset();
                result.page.reset();
                result.state = FilesPaneReplyState::LimitExceeded;
            }
            reply = std::move(result);
            needed = signal();
        }
        notify(needed);
    }
    void finish(FilesPaneWorkerFault reason) noexcept {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            worker_fault.store(reason, std::memory_order_release);
            busy = navigation_busy = change_busy = false;
            navigation.reset();
            read.reset();
            change.reset();
            preview.reset();
            operation.reset();
            active_stop.reset();
            if (!stopping && reason != FilesPaneWorkerFault::None) {
                // This path allocates nothing, even after std::bad_alloc.
                FilesPaneReply fatal;
                fatal.request = latest_request;
                fatal.navigation = latest_navigation;
                fatal.state = FilesPaneReplyState::WorkerFault;
                fatal.kind = FilesPaneReplyKind::Fault;
                fatal.worker_fault = reason;
                fatal.change_outcome = FilesPaneChangeOutcome::Unknown;
                reply = std::move(fatal);
            }
            alive.store(false, std::memory_order_release);
            needed = reason != FilesPaneWorkerFault::None ? (signalled = true) : signal();
        }
        // A notification failure cannot escape a thread entry or hide the fault.
        try {
            notify(needed);
        } catch (...) {
            if (reason == FilesPaneWorkerFault::None)
                worker_fault.store(FilesPaneWorkerFault::Exception);
        }
    }
    void run() noexcept {
        try {
            loop();
            finish(FilesPaneWorkerFault::None);
        } catch (const std::bad_alloc &) {
            finish(FilesPaneWorkerFault::Allocation);
        } catch (...) {
            finish(FilesPaneWorkerFault::Exception);
        }
    }
    void loop() {
        // All native owners and all index/model retirement stay in this scope.
        auto pane = std::make_unique<IndexedPane>();
        std::unique_ptr<BasicFileSystem> filesystem;
        std::shared_ptr<IndexFileFactory> factory;
        auto budget = std::make_shared<IndexStorageBudget>(limits.cache_bytes, limits.cache_files);
        std::shared_ptr<const FilesPaneLocation> active;
        FilesPaneTicket active_epoch;
        for (;;) {
            std::optional<Navigate> nav;
            std::optional<Read> page;
            std::optional<Change> edit;
            std::optional<Preview> inspection;
            std::optional<Operation> op;
            std::stop_source stop;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || navigation || change || read || preview || operation; });
                if (stopping)
                    return;
                if (navigation) {
                    nav = std::move(navigation);
                    navigation.reset();
                } else if (change) {
                    edit = std::move(change);
                    change.reset();
                } else if (preview) {
                    inspection = std::move(preview);
                    preview.reset();
                } else if (operation) {
                    // Operation preparation is serialized with all pane
                    // activity and hands native owners directly to the
                    // planning worker through its callback.
                    op = std::move(operation);
                    operation.reset();
                } else {
                    page = std::move(read);
                    read.reset();
                }
                busy = true;
                active_stop = stop;
                FilesPaneTicket current;
                if (nav)
                    current = nav->ticket;
                else if (edit)
                    current = edit->request.ticket;
                else if (inspection)
                    current = inspection->ticket;
                else if (page)
                    current = page->ticket;
                else
                    std::visit([&](const auto &request) { current = request.ticket; }, *op);
                if (current == cancelled)
                    stop.request_stop();
            }
            if (nav) {
                const auto *absolute = std::get_if<FilesPaneLocation>(&nav->destination);
                std::shared_ptr<const FilesPaneLocation> requested;
                if (absolute)
                    requested = std::make_shared<const FilesPaneLocation>(*absolute);
                FilesPaneReply result;
                result.kind = FilesPaneReplyKind::Navigation;
                result.request = result.navigation = nav->ticket;
                result.active_location = active;
                result.requested_location = requested;
                auto candidate = std::make_unique<IndexedPane>();
                bool committed = false;
                auto request = candidate->begin_listing(nav->view);
                const auto fail = [&](const UnavailableObservation &error,
                                      FilesPaneReplyState state = FilesPaneReplyState::Failed) {
                    candidate->fail_listing(request, error);
                    result.state = state;
                    result.error = error;
                };
                if (std::holds_alternative<HomeNavigation>(nav->destination) && !stop.stop_requested()) {
                    auto home =
                        backend.home
                            ? backend.home({limits.path_bytes}, stop.get_token())
                            : Observation<NativeAbsolutePath>::unavailable(
                                  ObservationState::Unsupported,
                                  {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery),
                                   "home.resolver_unavailable"});
                    if (!home.value()) {
                        const auto &e = *home.unavailable_detail();
                        fail(e, e.error.domain == ErrorDomain::Adapter &&
                                        e.error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit)
                                    ? FilesPaneReplyState::LimitExceeded
                                    : FilesPaneReplyState::Failed);
                    } else if (!home.value()->is_valid()) {
                        fail({ObservationState::Failed,
                              {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath),
                               "home.invalid_address"}});
                    } else {
                        const auto &value = *home.value();
                        const auto bytes = value.posix_bytes() ? value.posix_bytes()->size()
                                                               : value.windows_units()->size() * sizeof(char16_t);
                        if (bytes > limits.path_bytes)
                            fail({ObservationState::Unknown,
                                  {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit),
                                   "home.path_bytes"}},
                                 FilesPaneReplyState::LimitExceeded);
                        else {
                            nav->destination = FilesPaneLocation(std::move(home).take_value());
                            absolute = std::get_if<FilesPaneLocation>(&nav->destination);
                            requested = std::make_shared<const FilesPaneLocation>(*absolute);
                            result.requested_location = requested;
                        }
                    }
                }
                const auto *origin = std::get_if<ChildNavigation>(&nav->destination);
                const bool origin_ready =
                    !origin || (active && origin->navigation == active_epoch && pane->accepts(origin->stamp));
                if (!origin_ready)
                    result.state = FilesPaneReplyState::Stale;
                if (!stop.stop_requested() && origin_ready && !result.error) {
                    if (!filesystem) {
                        filesystem = backend.filesystem();
                        if (!filesystem)
                            throw std::runtime_error("null pane filesystem");
                    }
                    if (!factory) {
                        auto opened = backend.workspace();
                        if (!opened.value())
                            fail(*opened.unavailable_detail());
                        else {
                            factory = *opened.value();
                            if (!factory)
                                throw std::runtime_error("null pane workspace");
                        }
                    }
                    if (factory && !result.error && !stop.stop_requested()) {
                        std::unique_ptr<DirectoryCursor> directory;
                        if (absolute) {
                            auto opened = filesystem->open_native_directory(absolute->root_address());
                            if (!opened.value())
                                fail(*opened.unavailable_detail());
                            else
                                directory = std::move(opened).take_value();
                            for (const auto &name : absolute->components()) {
                                if (!directory || stop.stop_requested())
                                    break;
                                auto child = directory->open_directory_child(name);
                                if (!child.value()) {
                                    fail(*child.unavailable_detail());
                                    directory.reset();
                                    break;
                                }
                                directory = std::move(child).take_value();
                                if (!directory)
                                    throw std::runtime_error("null route directory");
                            }
                        } else {
                            const auto &child = std::get<ChildNavigation>(nav->destination);
                            if (!active || child.navigation != active_epoch || !pane->accepts(child.stamp))
                                result.state = FilesPaneReplyState::Stale;
                            else {
                                auto opened = pane->open_directory_row(
                                    child.stamp, child.row,
                                    {limits.page_wire_bytes, limits.path_bytes, limits.diagnostic_bytes},
                                    stop.get_token());
                                if (!opened.value()) {
                                    const auto &e = *opened.unavailable_detail();
                                    const auto state =
                                        !pane->accepts(child.stamp) ? FilesPaneReplyState::Stale
                                        : e.error.domain == ErrorDomain::Adapter &&
                                                e.error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit)
                                            ? FilesPaneReplyState::LimitExceeded
                                            : FilesPaneReplyState::Failed;
                                    fail(e, state);
                                } else {
                                    auto bound = std::move(opened).take_value();
                                    auto route =
                                        active->descend(bound.name, {limits.path_bytes}, limits.location_components);
                                    if (!route.value())
                                        fail(*route.unavailable_detail(),
                                             route.unavailable_detail()->error.code ==
                                                     static_cast<std::int64_t>(AdapterError::InventoryLimit)
                                                 ? FilesPaneReplyState::LimitExceeded
                                                 : FilesPaneReplyState::Failed);
                                    else {
                                        requested =
                                            std::make_shared<const FilesPaneLocation>(std::move(route).take_value());
                                        result.requested_location = requested;
                                        directory = std::move(bound.directory);
                                    }
                                }
                            }
                        }
                        if (directory && !stop.stop_requested()) {
                            auto listing = std::make_unique<DirectoryListing>(std::move(directory),
                                                                              DirectoryListingLimits{}, backend.clock);
                            std::array<std::byte, 16> generation{};
                            auto number = nav->ticket.number();
                            for (std::size_t i = 0; i < 8; ++i) {
                                generation[i] = std::byte(number & 255);
                                number >>= 8;
                            }
                            DirectoryIndexStoreLimits store;
                            store.rows = limits.directory_rows;
                            store.bytes = limits.cache_bytes;
                            DirectoryIndexListingJob collection(request, std::move(listing), backend.storage_model,
                                                                factory, budget, generation, store);
                            while (!stop.stop_requested()) {
                                const auto state = collection.advance(stop.get_token());
                                if (state == DirectoryIndexListingState::Complete ||
                                    state == DirectoryIndexListingState::Failed ||
                                    state == DirectoryIndexListingState::Cancelled)
                                    break;
                            }
                            auto collected = collection.completed();
                            if (collection.state() == DirectoryIndexListingState::Failed)
                                fail(*collected.unavailable_detail());
                            else if (collected.value() && !stop.stop_requested()) {
                                IndexedPaneDataJob preparation(*collected.value(), nav->view, {}, {},
                                                               limits.cache_bytes);
                                while (!stop.stop_requested()) {
                                    auto state = preparation.advance(stop.get_token());
                                    if (state == IndexedPaneDataJobState::Complete ||
                                        state == IndexedPaneDataJobState::Failed ||
                                        state == IndexedPaneDataJobState::Cancelled)
                                        break;
                                }
                                auto prepared = preparation.completed();
                                if (preparation.state() == IndexedPaneDataJobState::Failed)
                                    fail(*prepared.unavailable_detail());
                                else if (prepared.value() && !stop.stop_requested()) {
                                    const auto published = candidate->publish(*prepared.value(), stop.get_token());
                                    if (published == IndexedPanePublish::Applied) {
                                        capture(*candidate, candidate->state_stamp(), {0, nav->rows}, stop.get_token(),
                                                result);
                                        if (result.state == FilesPaneReplyState::Ready) {
                                            // No native I/O or native-owner destruction under this lock.
                                            std::lock_guard lock(mutex);
                                            if (!stopping && latest_request == nav->ticket &&
                                                cancelled != nav->ticket && !stop.stop_requested()) {
                                                pane.swap(candidate);
                                                active = requested;
                                                result.active_location = active;
                                                committed = true;
                                            } else {
                                                result.state = FilesPaneReplyState::Cancelled;
                                                result.page.reset();
                                            }
                                        }
                                    } else if (published == IndexedPanePublish::Failed && candidate->listing_error())
                                        fail(*candidate->listing_error());
                                    else if (published != IndexedPanePublish::Cancelled)
                                        throw std::runtime_error("pane publication invariant");
                                }
                            }
                        }
                    }
                }
                if (!committed && stop.stop_requested() &&
                    (!result.error ||
                     (result.error->error.domain == ErrorDomain::Adapter &&
                      result.error->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead)))) {
                    candidate->cancel_listing(request);
                    result.state = FilesPaneReplyState::Cancelled;
                    result.page.reset();
                }
                // Retirement may block in native close/unlink. Complete it on
                // the worker, outside the commit lock, before acknowledging a
                // replacement (including revocation of old display stamps).
                candidate.reset();
                result.stamp = pane->state_stamp();
                active_epoch = nav->ticket;
                emit(std::move(result), true);
            } else if (edit) {
                FilesPaneReply result;
                result.request = edit->request.ticket;
                result.navigation = edit->request.navigation;
                result.active_location = result.requested_location = active;
                apply(*pane, *edit, stop.get_token(), result);
                result.stamp = pane->state_stamp();
                emit(std::move(result), false, true);
            } else if (inspection) {
                FilesPaneReply result;
                result.request = inspection->ticket;
                result.navigation = inspection->navigation;
                result.active_location = result.requested_location = active;
                result.kind = FilesPaneReplyKind::Preview;
                if (!pane->data() || !pane->accepts(inspection->stamp)) {
                    result.state = FilesPaneReplyState::Stale;
                } else if (stop.stop_requested()) {
                    result.state = FilesPaneReplyState::Cancelled;
                } else {
                    auto opened = pane->open_row(inspection->stamp, inspection->row, ck::files::EntryAccess::ContentRead,
                                                 {limits.page_wire_bytes, limits.path_bytes, limits.diagnostic_bytes},
                                                 stop.get_token());
                    if (!opened.value()) {
                        result.state = FilesPaneReplyState::Failed;
                        result.error = *opened.unavailable_detail();
                    } else {
                        auto content = (*opened.value()).child->read_content({inspection->limits.bytes}, 0, stop.get_token());
                        if (!content.value()) {
                            result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled : FilesPaneReplyState::Failed;
                            result.error = *content.unavailable_detail();
                        } else {
                            result.state = FilesPaneReplyState::Ready;
                            auto value = std::move(content).take_value();
                            result.preview = std::make_shared<const FilesPanePreview>(
                                FilesPanePreview{std::move(value.bytes), value.complete});
                        }
                    }
                }
                result.stamp = pane->state_stamp();
                emit(std::move(result), false);
            } else if (op) {
                std::visit(
                    [&](auto &request) {
                        using Request = std::decay_t<decltype(request)>;
                        if constexpr (std::is_same_v<Request, OperationSources>) {
                            using Result = Observation<FilesPaneOperationSources>;
                            const auto fail = [&](UnavailableObservation detail) {
                                request.handoff(Result::unavailable(detail.state, detail.error));
                            };
                            if (!request.handoff)
                                throw std::invalid_argument("missing operation source handoff");
                            if (!pane->data() || request.navigation != active_epoch || !pane->accepts(request.stamp)) {
                                fail(operation_error(AdapterError::InvalidAccess, "pane.operation_sources.stale",
                                                      ObservationState::Changed));
                                return;
                            }
                            if (stop.stop_requested()) {
                                fail(operation_error(AdapterError::InterruptedRead, "pane.operation_sources.cancelled",
                                                      ObservationState::Unknown));
                                return;
                            }
                            auto started = pane->begin_source_acquisition(request.stamp, request.limits);
                            if (!started.value()) {
                                fail(*started.unavailable_detail());
                                return;
                            }
                            auto job = std::move(started).take_value();
                            auto state = job->state();
                            while (state == IndexedSourceJobState::Running && !stop.stop_requested())
                                state = job->advance(stop.get_token());
                            if (state != IndexedSourceJobState::Complete || stop.stop_requested()) {
                                if (stop.stop_requested()) {
                                    fail(operation_error(AdapterError::InterruptedRead,
                                                          "pane.operation_sources.cancelled", ObservationState::Unknown));
                                } else if (job->failure()) {
                                    fail(*job->failure());
                                } else {
                                    fail(operation_error(AdapterError::PlanInvalid, "pane.operation_sources.incomplete"));
                                }
                                return;
                            }
                            auto completed = job->completed();
                            if (!completed.value() || !pane->accepts_sources(**completed.value())) {
                                fail(operation_error(AdapterError::InvalidAccess, "pane.operation_sources.changed",
                                                      ObservationState::Changed));
                                return;
                            }
                            if (!active) {
                                fail(operation_error(AdapterError::InvalidAccess, "pane.operation_sources.location",
                                                      ObservationState::Changed));
                                return;
                            }
                            auto parent_path = active->root_address();
                            for (const auto &component : active->components()) {
                                auto child = parent_path.child_address(component, {limits.path_bytes});
                                if (!child.value()) {
                                    fail(*child.unavailable_detail());
                                    return;
                                }
                                parent_path = std::move(child).take_value();
                            }
                            FilesPaneOperationSources result;
                            const auto &sources = **completed.value();
                            result.sources.reserve(sources.sources().size());
                            result.paths.reserve(sources.sources().size());
                            result.names.reserve(sources.sources().size());
                            for (const auto &bound : sources.sources()) {
                                if (!bound.held()) {
                                    fail(operation_error(AdapterError::UnqualifiedIdentity,
                                                          "pane.operation_sources.handle", ObservationState::Unknown));
                                    return;
                                }
                                auto child = parent_path.child_address(bound.held()->name(), {limits.path_bytes});
                                if (!child.value()) {
                                    fail(*child.unavailable_detail());
                                    return;
                                }
                                result.sources.push_back(bound.held());
                                result.paths.push_back(std::move(child).take_value());
                                result.names.push_back(bound.held()->name());
                            }
                            if (result.sources.size() == 1 && result.sources.front()->metadata().kind.value() &&
                                *result.sources.front()->metadata().kind.value() == ObjectKind::Directory) {
                                auto opened = result.sources.front()->parent()->open_directory_child(result.names.front());
                                if (!opened.value()) {
                                    fail(*opened.unavailable_detail());
                                    return;
                                }
                                auto root = std::shared_ptr<DirectoryCursor>(std::move(opened).take_value());
                                if (!root || !root->identity().value() || !result.sources.front()->metadata().identity.value() ||
                                    compare_identity(*root->identity().value(),
                                                     *result.sources.front()->metadata().identity.value()) !=
                                        IdentityComparison::Same) {
                                    fail(operation_error(AdapterError::MetadataChanged,
                                                          "pane.operation_sources.directory_identity",
                                                          ObservationState::Changed));
                                    return;
                                }
                                result.root_source = result.sources.front();
                                result.source_root = std::move(root);
                                result.source_path = result.paths.front();
                                result.sources.clear();
                                result.paths.clear();
                            }
                            request.handoff(Result::available(std::move(result)));
                        } else if constexpr (std::is_same_v<Request, OperationDestination>) {
                            using Result = Observation<FilesPaneOperationDestination>;
                            if (!request.handoff)
                                throw std::invalid_argument("missing operation destination handoff");
                            if (!pane->data() || request.navigation != active_epoch || !pane->accepts(request.stamp)) {
                                request.handoff(Result::unavailable(ObservationState::Changed,
                                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess),
                                     "pane.operation_destination.stale"}));
                                return;
                            }
                            if (stop.stop_requested()) {
                                request.handoff(Result::unavailable(ObservationState::Unknown,
                                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead),
                                     "pane.operation_destination.cancelled"}));
                                return;
                            }
                            auto directory = pane->hold_directory(request.stamp);
                            if (!directory.value()) {
                                request.handoff(Result::unavailable(directory.unavailable_detail()->state,
                                    directory.unavailable_detail()->error));
                                return;
                            }
                            auto root_address = active->root_address().namespace_root();
                            if (!root_address.value()) {
                                request.handoff(Result::unavailable(root_address.unavailable_detail()->state,
                                    root_address.unavailable_detail()->error));
                                return;
                            }
                            auto opened_root = filesystem->open_native_directory(*root_address.value());
                            if (!opened_root.value()) {
                                request.handoff(Result::unavailable(opened_root.unavailable_detail()->state,
                                    opened_root.unavailable_detail()->error));
                                return;
                            }
                            auto provider_root = std::shared_ptr<const DirectoryCursor>(
                                std::move(opened_root).take_value());
                            if (!provider_root || !provider_root->identity().value()) {
                                request.handoff(Result::unavailable(ObservationState::Unknown,
                                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity),
                                     "pane.operation_destination.provider_root_identity"}));
                                return;
                            }
                            auto destination_address = active->root_address();
                            for (const auto &component : active->components()) {
                                auto child = destination_address.child_address(component, {limits.path_bytes});
                                if (!child.value()) {
                                    request.handoff(Result::unavailable(child.unavailable_detail()->state,
                                        child.unavailable_detail()->error));
                                    return;
                                }
                                destination_address = std::move(child).take_value();
                            }
                            request.handoff(Result::available({std::move(directory).take_value(),
                                                              std::move(provider_root),
                                                              std::optional<NativeAbsolutePath>(std::move(destination_address))}));
                        } else if constexpr (std::is_same_v<Request, CreateDirectory>) {
                            FilesPaneReply result;
                            result.kind = FilesPaneReplyKind::Mutation;
                            result.request = request.ticket;
                            result.navigation = request.navigation;
                            result.active_location = active;
                            result.stamp = pane->state_stamp();
                            if (!pane->data() || !active || request.navigation != active_epoch ||
                                !pane->accepts(request.stamp) ||
                                request.name.encoding() != active->root_address().encoding()) {
                                result.state = FilesPaneReplyState::Stale;
                            } else if (stop.stop_requested()) {
                                result.state = FilesPaneReplyState::Cancelled;
                            } else {
                                auto parent = pane->hold_directory(request.stamp);
                                if (!parent.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = *parent.unavailable_detail();
                                } else {
                                    auto created = (*parent.value())->create_child_directory(request.name);
                                    if (!created.value()) {
                                        result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                              : FilesPaneReplyState::Failed;
                                        result.error = *created.unavailable_detail();
                                    } else {
                                        result.state = FilesPaneReplyState::Ready;
                                        result.mutation_applied = true;
                                    }
                                }
                            }
                            emit(std::move(result), false);
                        } else if constexpr (std::is_same_v<Request, CreateFile>) {
                            FilesPaneReply result;
                            result.kind = FilesPaneReplyKind::Mutation;
                            result.request = request.ticket;
                            result.navigation = request.navigation;
                            result.active_location = active;
                            result.stamp = pane->state_stamp();
                            if (!pane->data() || !active || request.navigation != active_epoch ||
                                !pane->accepts(request.stamp) ||
                                request.name.encoding() != active->root_address().encoding()) {
                                result.state = FilesPaneReplyState::Stale;
                            } else if (stop.stop_requested()) {
                                result.state = FilesPaneReplyState::Cancelled;
                            } else {
                                auto parent = pane->hold_directory(request.stamp);
                                if (!parent.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = *parent.unavailable_detail();
                                } else {
                                    auto created = (*parent.value())->create_child_file(request.name);
                                    if (!created.value()) {
                                        result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                              : FilesPaneReplyState::Failed;
                                        result.error = *created.unavailable_detail();
                                    } else {
                                        auto flushed = (*created.value())->flush_content();
                                        if (!flushed.value()) {
                                            result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                                  : FilesPaneReplyState::Failed;
                                            result.error = *flushed.unavailable_detail();
                                        } else {
                                            result.state = FilesPaneReplyState::Ready;
                                            result.mutation_applied = true;
                                        }
                                    }
                                }
                            }
                            emit(std::move(result), false);
                        } else if constexpr (std::is_same_v<Request, CreateHardLink>) {
                            FilesPaneReply result;
                            result.kind = FilesPaneReplyKind::Mutation;
                            result.request = request.ticket;
                            result.navigation = request.navigation;
                            result.active_location = active;
                            result.stamp = pane->state_stamp();
                            if (!pane->data() || !active || request.navigation != active_epoch ||
                                !pane->accepts(request.stamp) ||
                                request.name.encoding() != active->root_address().encoding()) {
                                result.state = FilesPaneReplyState::Stale;
                            } else if (stop.stop_requested()) {
                                result.state = FilesPaneReplyState::Cancelled;
                            } else {
                                auto opened = pane->open_row(request.stamp, request.row, EntryAccess::MetadataOnly, {},
                                                             stop.get_token());
                                if (!opened.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = *opened.unavailable_detail();
                                } else if (!opened.value()->child || !opened.value()->child->metadata().identity.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = operation_error(AdapterError::UnqualifiedIdentity,
                                                                  "pane.create_hard_link.source_identity",
                                                                  ObservationState::Unknown);
                                } else if (!opened.value()->child->metadata().kind.value() ||
                                           *opened.value()->child->metadata().kind.value() != ObjectKind::RegularFile) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = operation_error(AdapterError::NotApplicable,
                                                                  "pane.create_hard_link.source_kind",
                                                                  ObservationState::Unsupported);
                                } else {
                                    auto parent = pane->hold_directory(request.stamp);
                                    if (!parent.value()) {
                                        result.state = FilesPaneReplyState::Failed;
                                        result.error = *parent.unavailable_detail();
                                    } else {
                                        auto linked = (*parent.value())->create_hard_link(
                                            opened.value()->name, *opened.value()->child->metadata().identity.value(),
                                            **parent.value(), request.name);
                                        if (!linked.value()) {
                                            result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                                 : FilesPaneReplyState::Failed;
                                            result.error = *linked.unavailable_detail();
                                        } else {
                                            result.state = FilesPaneReplyState::Ready;
                                            result.mutation_applied = true;
                                        }
                                    }
                                }
                            }
                            emit(std::move(result), false);
                        } else if constexpr (std::is_same_v<Request, CreateSymbolicLink>) {
                            FilesPaneReply result;
                            result.kind = FilesPaneReplyKind::Mutation;
                            result.request = request.ticket;
                            result.navigation = request.navigation;
                            result.active_location = active;
                            result.stamp = pane->state_stamp();
                            if (!pane->data() || !active || request.navigation != active_epoch ||
                                !pane->accepts(request.stamp) ||
                                request.name.encoding() != active->root_address().encoding()) {
                                result.state = FilesPaneReplyState::Stale;
                            } else if (stop.stop_requested()) {
                                result.state = FilesPaneReplyState::Cancelled;
                            } else {
                                auto parent = pane->hold_directory(request.stamp);
                                if (!parent.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = *parent.unavailable_detail();
                                } else {
                                    auto created = (*parent.value())->create_symbolic_link(request.target, request.name);
                                    if (!created.value()) {
                                        result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                              : FilesPaneReplyState::Failed;
                                        result.error = *created.unavailable_detail();
                                    } else {
                                        result.state = FilesPaneReplyState::Ready;
                                        result.mutation_applied = true;
                                    }
                                }
                            }
                            emit(std::move(result), false);
                        } else if constexpr (std::is_same_v<Request, DeleteChild> ||
                                             std::is_same_v<Request, TrashChild>) {
                            FilesPaneReply result;
                            result.kind = FilesPaneReplyKind::Mutation;
                            result.request = request.ticket;
                            result.navigation = request.navigation;
                            result.active_location = active;
                            result.stamp = pane->state_stamp();
                            if (!pane->data() || !active || request.navigation != active_epoch ||
                                !pane->accepts(request.stamp)) {
                                result.state = FilesPaneReplyState::Stale;
                            } else if (stop.stop_requested()) {
                                result.state = FilesPaneReplyState::Cancelled;
                            } else {
                                auto opened = pane->open_row(request.stamp, request.row, EntryAccess::MetadataOnly, {},
                                                             stop.get_token());
                                if (!opened.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = *opened.unavailable_detail();
                                } else if (!opened.value()->child || !opened.value()->child->metadata().identity.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = operation_error(AdapterError::UnqualifiedIdentity,
                                                                  "pane.delete.source_identity",
                                                                  ObservationState::Unknown);
                                } else {
                                    auto parent = pane->hold_directory(request.stamp);
                                    if (!parent.value()) {
                                        result.state = FilesPaneReplyState::Failed;
                                        result.error = *parent.unavailable_detail();
                                    } else {
                                        auto retired = [&] {
                                            if constexpr (std::is_same_v<Request, TrashChild>)
                                                return (*parent.value())->trash_child(
                                                    opened.value()->name, *opened.value()->child->metadata().identity.value());
                                            else
                                                return (*parent.value())->retire_child(
                                                    opened.value()->name, *opened.value()->child->metadata().identity.value());
                                        }();
                                        if (!retired.value()) {
                                            result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                                  : FilesPaneReplyState::Failed;
                                            result.error = *retired.unavailable_detail();
                                        } else {
                                            result.state = FilesPaneReplyState::Ready;
                                            result.mutation_applied = true;
                                        }
                                    }
                                }
                            }
                            emit(std::move(result), false);
                        } else {
                            FilesPaneReply result;
                            result.kind = FilesPaneReplyKind::Mutation;
                            result.request = request.ticket;
                            result.navigation = request.navigation;
                            result.active_location = active;
                            result.stamp = pane->state_stamp();
                            if (!pane->data() || !active || request.navigation != active_epoch ||
                                !pane->accepts(request.stamp) ||
                                request.name.encoding() != active->root_address().encoding()) {
                                result.state = FilesPaneReplyState::Stale;
                            } else if (stop.stop_requested()) {
                                result.state = FilesPaneReplyState::Cancelled;
                            } else {
                                auto opened =
                                    pane->open_row(request.stamp, request.row, EntryAccess::MetadataOnly, {}, stop.get_token());
                                if (!opened.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = *opened.unavailable_detail();
                                } else if (!opened.value()->child || !opened.value()->child->metadata().identity.value()) {
                                    result.state = FilesPaneReplyState::Failed;
                                    result.error = operation_error(AdapterError::UnqualifiedIdentity,
                                                                  "pane.rename.source_identity",
                                                                  ObservationState::Unknown);
                                } else {
                                    auto parent = pane->hold_directory(request.stamp);
                                    if (!parent.value()) {
                                        result.state = FilesPaneReplyState::Failed;
                                        result.error = *parent.unavailable_detail();
                                    } else {
                                        auto renamed = (*parent.value())->rename_child_noreplace(
                                            opened.value()->name, *opened.value()->child->metadata().identity.value(),
                                            **parent.value(), request.name);
                                        if (!renamed.value() && renamed.state() == ObservationState::Changed)
                                            renamed = (*parent.value())->rename_child_case_only(
                                                opened.value()->name, *opened.value()->child->metadata().identity.value(), request.name);
                                        if (!renamed.value()) {
                                            result.state = stop.stop_requested() ? FilesPaneReplyState::Cancelled
                                                                                  : FilesPaneReplyState::Failed;
                                            result.error = *renamed.unavailable_detail();
                                        } else {
                                            result.state = FilesPaneReplyState::Ready;
                                            result.mutation_applied = true;
                                        }
                                    }
                                }
                            }
                            emit(std::move(result), false);
                        }
                    },
                    *op);
                {
                    std::lock_guard lock(mutex);
                    busy = false;
                    active_stop.reset();
                }
            } else {
                FilesPaneReply result;
                result.request = page->ticket;
                result.navigation = page->navigation;
                result.active_location = result.requested_location = active;
                capture(*pane, page->stamp, page->range, stop.get_token(), result);
                result.stamp = pane->state_stamp();
                emit(std::move(result), false);
            }
        }
    }
    void apply(IndexedPane &pane, const Change &edit, std::stop_token stop, FilesPaneReply &result) {
        const auto &request = edit.request;
        result.change_outcome = FilesPaneChangeOutcome::NotApplied;
        result.kind = std::holds_alternative<FilesPaneFocusRow>(edit.command)      ? FilesPaneReplyKind::Focus
                      : std::holds_alternative<FilesPaneChangeMarks>(edit.command) ? FilesPaneReplyKind::Marks
                                                                                   : FilesPaneReplyKind::View;
        if (!pane.data() || !pane.accepts(request.stamp)) {
            result.state = FilesPaneReplyState::Stale;
            return;
        }
        if (stop.stop_requested()) {
            result.state = FilesPaneReplyState::Cancelled;
            return;
        }
        if (const auto *focus = std::get_if<FilesPaneFocusRow>(&edit.command)) {
            switch (pane.focus(request.stamp, focus->row, stop)) {
            case IndexedPaneFocus::Applied:
                result.change_outcome = FilesPaneChangeOutcome::Applied;
                break;
            case IndexedPaneFocus::Stale:
                result.state = FilesPaneReplyState::Stale;
                break;
            case IndexedPaneFocus::Cancelled:
                result.state = FilesPaneReplyState::Cancelled;
                break;
            case IndexedPaneFocus::Failed:
                result.error = pane.listing_error();
                break;
            case IndexedPaneFocus::Unavailable:
            case IndexedPaneFocus::InvalidTarget:
                break;
            }
        } else if (const auto *marks = std::get_if<FilesPaneChangeMarks>(&edit.command)) {
            auto started = pane.begin_mark_change(request.stamp, marks->command, 256, limits.cache_bytes, stop);
            if (!started.value())
                result.error = *started.unavailable_detail();
            else {
                auto job = std::move(started).take_value();
                const auto stamp = job->stamp();
                auto state = job->state();
                while (state == IndexedMarkJobState::Running)
                    state = job->advance(stop);
                if (state == IndexedMarkJobState::Complete && !stop.stop_requested()) {
                    auto completed = job->completed();
                    if (!completed.value() || pane.publish_marks(*completed.value()) != IndexedPanePublish::Applied)
                        throw std::logic_error("pane mark publication invariant");
                    result.change_outcome = FilesPaneChangeOutcome::Applied;
                } else if (state == IndexedMarkJobState::Failed) {
                    result.error = *job->failure();
                    pane.fail_marks(stamp, *result.error);
                } else {
                    pane.cancel_marks(stamp);
                    result.state = FilesPaneReplyState::Cancelled;
                }
            }
        } else {
            const auto &view = std::get<FilesPaneChangeView>(edit.command).view;
            auto started = pane.begin_view_change(request.stamp, view);
            if (!started.value())
                result.error = *started.unavailable_detail();
            else {
                const auto listing = *started.value();
                IndexedPaneDataJob job(pane.data(), listing, view, {}, {}, limits.cache_bytes, pane.marks());
                auto state = job.state();
                while (state == IndexedPaneDataJobState::Sorting || state == IndexedPaneDataJobState::Indexing ||
                       state == IndexedPaneDataJobState::CountingMarks)
                    state = job.advance(stop);
                if (state == IndexedPaneDataJobState::Complete && !stop.stop_requested()) {
                    auto completed = job.completed();
                    if (!completed.value())
                        throw std::logic_error("missing prepared pane view");
                    const auto published = pane.publish(*completed.value(), stop);
                    if (published == IndexedPanePublish::Applied)
                        result.change_outcome = FilesPaneChangeOutcome::Applied;
                    else if (published == IndexedPanePublish::Failed) {
                        result.error = pane.listing_error();
                        if (!result.error)
                            throw std::logic_error("missing pane view failure");
                        pane.fail_listing(listing, *result.error);
                    } else if (published == IndexedPanePublish::Cancelled) {
                        pane.cancel_listing(listing);
                        result.state = FilesPaneReplyState::Cancelled;
                    } else
                        throw std::logic_error("pane view publication invariant");
                } else if (state == IndexedPaneDataJobState::Failed) {
                    result.error = *job.failure();
                    pane.fail_listing(listing, *result.error);
                } else {
                    pane.cancel_listing(listing);
                    result.state = FilesPaneReplyState::Cancelled;
                }
            }
        }
        // Reconcile the page with the final state, including after rollback of
        // a pending mark/view job. Keep the command failure distinct from the
        // recovery page; cancellation never turns into apparent success.
        if (result.change_outcome == FilesPaneChangeOutcome::NotApplied && stop.stop_requested() && result.error &&
            result.error->error.domain == ErrorDomain::Adapter &&
            result.error->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead))
            result.state = FilesPaneReplyState::Cancelled;
        auto range = request.range;
        const auto total = pane.visible_rows();
        range.first = std::min(range.first, total > range.rows ? total - range.rows : 0);
        if (result.kind != FilesPaneReplyKind::Marks)
            if (const auto focus = pane.focused_position()) {
                if (*focus < range.first)
                    range.first = *focus;
                else if (*focus - range.first >= range.rows)
                    range.first = *focus - (range.rows - 1);
            }
        FilesPaneReply display;
        capture(pane, pane.state_stamp(), range, stop, display);
        if (result.change_outcome == FilesPaneChangeOutcome::Applied) {
            result.state = display.state;
            result.error = std::move(display.error);
        }
        result.page = std::move(display.page);
    }
    void capture(IndexedPane &pane, const PaneStateStamp &stamp, FilesPaneReadRange range, std::stop_token stop,
                 FilesPaneReply &result) {
        if (!pane.data() || !pane.accepts(stamp)) {
            result.state = FilesPaneReplyState::Stale;
            return;
        }
        IndexedPanePageLimits page_limits;
        page_limits.display = {range.rows, limits.page_text_bytes};
        page_limits.wire_bytes = limits.page_wire_bytes;
        auto captured = pane.capture_page(stamp, range.first, page_limits, stop);
        switch (captured.state) {
        case IndexedPaneCaptureState::Ready:
            if (!captured.page)
                throw std::logic_error("missing completed pane page");
            if (captured.page->rows().size() !=
                std::min<std::uint64_t>(range.rows, captured.page->visible_rows() - range.first)) {
                result.state = FilesPaneReplyState::LimitExceeded;
                return;
            }
            result.state = FilesPaneReplyState::Ready;
            result.page = std::move(captured.page);
            return;
        case IndexedPaneCaptureState::Stale:
            result.state = FilesPaneReplyState::Stale;
            return;
        case IndexedPaneCaptureState::OutOfRange:
            result.state = FilesPaneReplyState::OutOfRange;
            return;
        case IndexedPaneCaptureState::LimitExceeded:
            result.state = FilesPaneReplyState::LimitExceeded;
            return;
        case IndexedPaneCaptureState::Cancelled:
            result.state = FilesPaneReplyState::Cancelled;
            return;
        case IndexedPaneCaptureState::Failed:
            result.state = FilesPaneReplyState::Failed;
            result.error = pane.listing_error();
            return;
        }
        throw std::runtime_error("pane capture invariant");
    }
};
FilesPaneBackend native_files_pane_backend(NativeAbsolutePath cache_root) {
    if (!cache_root.is_valid())
        throw std::invalid_argument("invalid pane cache address");
    // The two native panes are views over one host filesystem provider. Share
    // its live comparison scope so worker-owned source and destination handles
    // can be compared safely; independently constructed NativeFileSystem
    // adapters retain their separate scopes.
    static const AdapterScope native_provider_scope;
    FilesPaneBackend backend;
    backend.filesystem = [scope = native_provider_scope] {
        return std::make_unique<NativeFileSystem>(scope);
    };
    backend.workspace = [root = std::move(cache_root)] { return open_native_index_workspace(root); };
    backend.home = [](NativePathLimits limits, std::stop_token stop) { return native_home_address(limits, stop); };
#if defined(_WIN32)
    backend.storage_model = StorageModel::WindowsHandleAttributes;
#elif defined(__APPLE__)
    backend.storage_model = StorageModel::DarwinStatFlags;
#elif defined(__linux__)
    backend.storage_model = StorageModel::LinuxStatx;
#else
    throw std::runtime_error("native file pane is unsupported on this platform");
#endif
    return backend;
}
FilesPaneService::FilesPaneService(FilesPaneBackend backend, FilesPaneServiceLimits limits,
                                   std::function<void()> wake) {
    if (!backend.filesystem || !backend.workspace || !backend.clock || !valid(limits) ||
        backend.storage_model < StorageModel::LinuxStatx ||
        backend.storage_model > StorageModel::WindowsHandleAttributes)
        throw std::invalid_argument("invalid pane service configuration");
    state_ = std::make_unique<State>(std::move(backend), limits, std::move(wake));
    state_->worker = std::jthread([state = state_.get()] { state->run(); });
}
FilesPaneService::~FilesPaneService() { join(); }
FilesPaneServiceLimits FilesPaneService::limits() const noexcept { return state_->limits; }
FilesPaneSubmitted FilesPaneService::navigate(NativeAbsolutePath location, PaneView view, std::size_t first_page_rows) {
    if (!location.is_valid())
        return {FilesPaneSubmission::Invalid, {}};
    return navigate(FilesPaneLocation(std::move(location)), std::move(view), first_page_rows);
}
FilesPaneSubmitted FilesPaneService::navigate(FilesPaneLocation location, PaneView view, std::size_t first_page_rows) {
    if (!location.root_address().is_valid() || !valid_pane_order({view.sort, view.descending, view.folders_first}))
        return {FilesPaneSubmission::Invalid, {}};
    if (location.native_bytes() > state_->limits.path_bytes ||
        location.components().size() > state_->limits.location_components || view.display_substring.size() > 4096 ||
        !first_page_rows || first_page_rows > state_->limits.page_rows)
        return {FilesPaneSubmission::LimitExceeded, {}};
    return state_->submit_navigation(std::move(location), std::move(view), first_page_rows);
}
FilesPaneSubmitted FilesPaneService::navigate_home(PaneView view, std::size_t first_page_rows) {
    if (!valid_pane_order({view.sort, view.descending, view.folders_first}))
        return {FilesPaneSubmission::Invalid, {}};
    if (view.display_substring.size() > 4096 || !first_page_rows || first_page_rows > state_->limits.page_rows)
        return {FilesPaneSubmission::LimitExceeded, {}};
    return state_->submit_navigation(HomeNavigation{}, std::move(view), first_page_rows);
}
FilesPaneSubmitted FilesPaneService::enter_directory(FilesPaneTicket navigation, PaneStateStamp stamp, PaneRowId row,
                                                     PaneView view, std::size_t first_page_rows) {
    if (!navigation.valid() || !row.scope.is_valid() || !row.number ||
        !valid_pane_order({view.sort, view.descending, view.folders_first}))
        return {FilesPaneSubmission::Invalid, {}};
    if (!first_page_rows || first_page_rows > state_->limits.page_rows || view.display_substring.size() > 4096)
        return {FilesPaneSubmission::LimitExceeded, {}};
    FilesPaneTicket request;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesPaneSubmission::Stopped, {}};
        if (navigation != state_->latest_navigation)
            return {FilesPaneSubmission::Invalid, {}};
        if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview || state_->reply)
            return {FilesPaneSubmission::Busy, {}};
        if (!state_->next)
            return {FilesPaneSubmission::Exhausted, {}};
        request = ticket(state_->next++);
        state_->navigation.emplace(Navigate{request, ChildNavigation{navigation, std::move(stamp), std::move(row)},
                                            std::move(view), first_page_rows});
        state_->latest_navigation = state_->latest_request = request;
        state_->cancelled = {};
        state_->navigation_busy = true;
    }
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::read_page(FilesPaneTicket navigation, PaneStateStamp stamp,
                                               FilesPaneReadRange range) {
    if (!navigation.valid())
        return {FilesPaneSubmission::Invalid, {}};
    if (!range.rows || range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::LimitExceeded, {}};
    std::optional<std::stop_source> stop;
    FilesPaneTicket request;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesPaneSubmission::Stopped, {}};
        if (navigation != state_->latest_navigation)
            return {FilesPaneSubmission::Invalid, {}};
        if (state_->navigation_busy || state_->change_busy || state_->operation || state_->reply)
            return {FilesPaneSubmission::Busy, {}};
        if (!state_->next)
            return {FilesPaneSubmission::Exhausted, {}};
        request = ticket(state_->next++);
        state_->read = Read{request, navigation, std::move(stamp), range};
        state_->latest_request = request;
        state_->cancelled = {};
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::preview(FilesPaneTicket navigation, PaneStateStamp stamp, PaneRowId row,
                                             FilesPanePreviewLimits limits) {
    if (!navigation.valid() || !row.scope.is_valid() || !row.number || !limits.valid())
        return {FilesPaneSubmission::Invalid, {}};
    FilesPaneTicket request;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesPaneSubmission::Stopped, {}};
        if (navigation != state_->latest_navigation)
            return {FilesPaneSubmission::Invalid, {}};
        if (state_->navigation_busy || state_->change_busy || state_->busy || state_->navigation || state_->read ||
            state_->change || state_->preview || state_->reply)
            return {FilesPaneSubmission::Busy, {}};
        if (!state_->next)
            return {FilesPaneSubmission::Exhausted, {}};
        request = ticket(state_->next++);
        state_->preview = Preview{request, navigation, std::move(stamp), std::move(row), limits};
        state_->latest_request = request;
        state_->cancelled = {};
    }
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::change(FilesPaneTicket navigation, PaneStateStamp stamp, FilesPaneChange command,
                                            FilesPaneReadRange range) {
    if (!navigation.valid() || command.valueless_by_exception())
        return {FilesPaneSubmission::Invalid, {}};
    if (!range.rows || range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::LimitExceeded, {}};
    if (const auto *view = std::get_if<FilesPaneChangeView>(&command)) {
        if (!valid_pane_order({view->view.sort, view->view.descending, view->view.folders_first}))
            return {FilesPaneSubmission::Invalid, {}};
        if (view->view.display_substring.size() > 4096)
            return {FilesPaneSubmission::LimitExceeded, {}};
    }
    FilesPaneTicket request;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesPaneSubmission::Stopped, {}};
        if (navigation != state_->latest_navigation)
            return {FilesPaneSubmission::Invalid, {}};
        if (state_->busy || state_->navigation || state_->read || state_->change || state_->reply)
            return {FilesPaneSubmission::Busy, {}};
        if (!state_->next)
            return {FilesPaneSubmission::Exhausted, {}};
        request = ticket(state_->next++);
        state_->change.emplace(Change{Read{request, navigation, std::move(stamp), range}, std::move(command)});
        state_->latest_request = request;
        state_->cancelled = {};
        state_->change_busy = true;
    }
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::create_directory(FilesPaneTicket navigation, PaneStateStamp stamp,
                                                       NativeName name, FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || !name.is_valid() || !range.rows ||
        range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(CreateDirectory{request, navigation, std::move(stamp), std::move(name), range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::create_file(FilesPaneTicket navigation, PaneStateStamp stamp,
                                                  NativeName name, FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || !name.is_valid() || !range.rows ||
        range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(CreateFile{request, navigation, std::move(stamp), std::move(name), range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::create_hard_link(FilesPaneTicket navigation, PaneStateStamp stamp,
                                                       PaneRowId row, NativeName name, FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || !row.number || !name.is_valid() || !range.rows ||
        range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(CreateHardLink{request, navigation, std::move(stamp), row, std::move(name), range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::create_symbolic_link(FilesPaneTicket navigation, PaneStateStamp stamp,
                                                           SymbolicLinkTarget target, NativeName name,
                                                           FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || std::visit([](const auto& value) {
            return value.empty() || std::find(value.begin(), value.end(), 0) != value.end();
        }, target) || !name.is_valid() || !range.rows || range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(CreateSymbolicLink{request, navigation, std::move(stamp), std::move(target),
                                                 std::move(name), range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::rename(FilesPaneTicket navigation, PaneStateStamp stamp, PaneRowId row,
                                             NativeName name, FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || !row.number || !name.is_valid() || !range.rows ||
        range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(RenameChild{request, navigation, std::move(stamp), row, std::move(name), range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::permanently_delete(FilesPaneTicket navigation, PaneStateStamp stamp,
                                                         PaneRowId row, FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || !row.number || !range.rows ||
        range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(DeleteChild{request, navigation, std::move(stamp), row, range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::trash(FilesPaneTicket navigation, PaneStateStamp stamp, PaneRowId row,
                                            FilesPaneReadRange range) {
    if (!navigation.valid() || !stamp.current() || !row.number || !range.rows ||
        range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation)
        return {FilesPaneSubmission::Invalid, {}};
    if (state_->busy || state_->navigation || state_->read || state_->change || state_->preview ||
        state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(TrashChild{request, navigation, std::move(stamp), row, range});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::acquire_operation_sources(FilesPaneTicket navigation, PaneStateStamp stamp,
                                                               IndexedSourceLimits limits,
                                                               FilesPaneOperationSourceHandoff handoff) {
    if (!navigation.valid() || !handoff || !stamp.current() || !limits.held_entries || limits.held_entries > 4096 ||
        !limits.native_name_bytes || limits.native_name_bytes > 64 * 1024 * 1024 || !limits.diagnostic_bytes ||
        limits.diagnostic_bytes > 16 * 1024 * 1024 || !limits.scan_rows || limits.scan_rows > 4096 ||
        limits.wire_bytes < 16 || limits.wire_bytes > DirectoryIndexScanLimits::hard_wire_bytes ||
        !valid_entry_access(limits.access))
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation || state_->busy || state_->navigation || state_->read ||
        state_->change || state_->preview || state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(OperationSources{request, navigation, std::move(stamp), limits, std::move(handoff)});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::acquire_operation_destination(
    FilesPaneTicket navigation, PaneStateStamp stamp, FilesPaneOperationDestinationHandoff handoff) {
    if (!navigation.valid() || !handoff || !stamp.current())
        return {FilesPaneSubmission::Invalid, {}};
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->alive.load())
        return {FilesPaneSubmission::Stopped, {}};
    if (navigation != state_->latest_navigation || state_->busy || state_->navigation || state_->read ||
        state_->change || state_->preview || state_->operation || state_->reply)
        return {FilesPaneSubmission::Busy, {}};
    if (!state_->next)
        return {FilesPaneSubmission::Exhausted, {}};
    const auto request = ticket(state_->next++);
    state_->operation.emplace(OperationDestination{request, navigation, std::move(stamp), std::move(handoff)});
    state_->latest_request = request;
    state_->cancelled = {};
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
bool FilesPaneService::cancel(const FilesPaneTicket &request) {
    std::optional<std::stop_source> stop;
    {
        std::lock_guard lock(state_->mutex);
        if (!request.valid() || request != state_->latest_request || state_->stopping ||
            (!state_->navigation && !state_->read && !state_->change && !state_->preview && !state_->operation &&
             !state_->busy))
            return false;
        state_->cancelled = request;
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    return true;
}
std::optional<FilesPaneReply> FilesPaneService::take() {
    std::lock_guard lock(state_->mutex);
    state_->signalled = false;
    auto result = std::move(state_->reply);
    state_->reply.reset();
    return result;
}
void FilesPaneService::request_stop() noexcept {
    std::optional<std::stop_source> stop;
    std::optional<Operation> queued_operation;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->navigation.reset();
        state_->read.reset();
        state_->change.reset();
        state_->preview.reset();
        queued_operation = std::move(state_->operation);
        state_->operation.reset();
        state_->reply.reset();
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    if (queued_operation)
        cancel_operation(*queued_operation);
    state_->condition.notify_one();
}
void FilesPaneService::join() {
    request_stop();
    if (state_->worker.joinable()) {
        if (state_->worker.get_id() == std::this_thread::get_id())
            throw std::logic_error("pane service cannot join itself");
        state_->worker.join();
    }
}
bool FilesPaneService::running() const noexcept { return state_->alive.load(std::memory_order_acquire); }
bool FilesPaneService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->navigation || state_->change || state_->read || state_->preview || state_->operation;
}
FilesPaneWorkerFault FilesPaneService::fault() const noexcept {
    return state_->worker_fault.load(std::memory_order_acquire);
}
} // namespace ck::vision
