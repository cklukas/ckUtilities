// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WP-07 reliability, fault-injection, and real process-crash test suite:
//   1. Index storage budget exhaustion (byte & file-slot budgets) and recovery
//      without corrupting prior published pane state.
//   2. Repeated rapid cancellation & controller/service lifetime stress across
//      FilesPaneService, FilesOperationPlanningService, and
//      FilesOperationExecutionService, including in-flight destruction.
//   3. Real process crash (SIGKILL on POSIX / TerminateProcess on Windows) at
//      destructive staging-copy and move-journal boundaries, verifying source &
//      destination byte preservation, fail-closed native-identity recovery
//      overlap detection, and unblocked disjoint transfers.
//   4. Low-memory / bounded & oversized input resilience for recovery reports,
//      overlap assessment, and FilesSettings.

#include "files_operation_execution_service.hpp"
#include "files_operation_planning_service.hpp"
#include "files_operation_recovery.hpp"
#include "files_pane_service.hpp"
#include "files_settings.hpp"

#include "directory_index_listing_job.hpp"
#include "directory_index_store.hpp"
#include "frozen_inventory.hpp"
#include "native_filesystem.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "operation_journal.hpp"
#include "operation_journal_file.hpp"
#include "scratch_directory.hpp"
#include "simulated_filesystem.hpp"
#include "native_operation_journal_id.hpp"
#include "sync_session.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "windows_name.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace {

using namespace ck::files;
using namespace ck::vision;

std::string g_argv0;

constexpr auto kSanitizerAwareTimeout =
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    std::chrono::seconds(30);
#else
    std::chrono::seconds(10);
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    std::chrono::seconds(30);
#else
    std::chrono::seconds(10);
#endif

template <class T>
T take_obs(Observation<T> value, const char *context) {
    if (!value.value()) {
        const auto detail = value.unavailable_detail();
        throw std::runtime_error(std::string(context) + ": " +
                                 (detail ? detail->error.operation : "unavailable"));
    }
    return std::move(value).take_value();
}

NativeAbsolutePath host_address(const std::filesystem::path &path) {
    return take_obs(NativeAbsolutePath::from_host(path), "host_address");
}

NativeAbsolutePath virtual_address(const char *leaf_name) {
#if defined(_WIN32)
    return take_obs(NativeAbsolutePath::from_host(std::filesystem::path(L"V:\\") / leaf_name), "virtual_address");
#else
    return take_obs(NativeAbsolutePath::from_host(std::filesystem::path("/") / leaf_name), "virtual_address");
#endif
}

NativeName native_leaf(const char *posix_text, const char16_t *win_text) {
#if defined(_WIN32)
    (void)posix_text;
    auto parsed = NativeName::from_windows(win_text);
#else
    (void)win_text;
    auto parsed = NativeName::from_posix(posix_text);
#endif
    if (!parsed)
        throw std::runtime_error("invalid fixture native leaf name");
    return std::move(*parsed);
}

NativeName native_leaf_ascii(std::string_view ascii) {
#if defined(_WIN32)
    std::u16string wide;
    wide.reserve(ascii.size());
    for (const char ch : ascii)
        wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(ch)));
    auto parsed = NativeName::from_windows(wide);
#else
    auto parsed = NativeName::from_posix(std::string(ascii));
#endif
    if (!parsed)
        throw std::runtime_error("invalid ascii native leaf name");
    return std::move(*parsed);
}

std::optional<std::string> get_env_string(const char *var_name) {
#if defined(_MSC_VER)
    char *raw = nullptr;
    std::size_t len = 0;
    const int err = ::_dupenv_s(&raw, &len, var_name);
    const std::unique_ptr<char, decltype(&std::free)> owned(raw, &std::free);
    if (err != 0 || !owned)
        return std::nullopt;
    return std::string(owned.get());
#else
    if (const char *raw = std::getenv(var_name))
        return std::string(raw);
    return std::nullopt;
#endif
}

// Cross-platform exact file read/write honoring AGENTS.md Windows wide/extended-path policy.
bool write_native_file_bytes(const std::filesystem::path &path, std::string_view content) {
#if defined(_WIN32)
    const auto extended = windows_extended_path(path.native());
    if (!extended)
        return false;
    HANDLE handle = CreateFileW(extended->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - offset, 64U * 1024U));
        DWORD written = 0;
        if (!WriteFile(handle, content.data() + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok && !FlushFileBuffers(handle))
        ok = false;
    if (!CloseHandle(handle))
        ok = false;
    return ok && offset == content.size();
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        const ssize_t n = ::write(fd, content.data() + offset, content.size() - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0)
        ok = false;
    if (::close(fd) != 0)
        ok = false;
    return ok && offset == content.size();
#endif
}

bool read_native_file_bytes(const std::filesystem::path &path, std::string &out) {
    out.clear();
#if defined(_WIN32)
    const auto extended = windows_extended_path(path.native());
    if (!extended)
        return false;
    HANDLE handle = CreateFileW(extended->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    char buffer[8192];
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        out.append(buffer, static_cast<std::size_t>(got));
    }
    if (!CloseHandle(handle))
        ok = false;
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buffer[8192];
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0)
            break;
        out.append(buffer, static_cast<std::size_t>(n));
    }
    if (::close(fd) != 0)
        ok = false;
    return ok;
#endif
}

bool create_private_dir(const std::filesystem::path &path) {
#if defined(_WIN32)
    const auto extended = windows_extended_path(path.native());
    if (!extended)
        return false;
    if (!CreateDirectoryW(extended->c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
#else
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    return ::chmod(path.c_str(), 0700) == 0;
#endif
}

// ---------------------------------------------------------------------------
// In-memory IndexByteFile / IndexFileFactory for deterministic budget testing
// ---------------------------------------------------------------------------

class MemoryIndexByteFile final : public IndexByteFile {
    std::vector<std::byte> bytes_;

  public:
    Observation<std::uint64_t> size() override {
        return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(bytes_.size()));
    }
    Observation<std::size_t> write_at(std::uint64_t at, std::span<const std::byte> data) override {
        const auto end = at + static_cast<std::uint64_t>(data.size());
        if (end < at)
            return Observation<std::size_t>::unavailable(
                ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit),
                                           "memory_index.overflow"});
        if (end > bytes_.size())
            bytes_.resize(static_cast<std::size_t>(end));
        if (!data.empty())
            std::memcpy(bytes_.data() + static_cast<std::size_t>(at), data.data(), data.size());
        return Observation<std::size_t>::available(data.size());
    }
    Observation<std::size_t> read_at(std::uint64_t at, std::span<std::byte> data) override {
        if (at >= bytes_.size() || data.empty())
            return Observation<std::size_t>::available(0U);
        const std::size_t avail = static_cast<std::size_t>(bytes_.size() - at);
        const std::size_t count = std::min(avail, data.size());
        std::memcpy(data.data(), bytes_.data() + static_cast<std::size_t>(at), count);
        return Observation<std::size_t>::available(count);
    }
};

class MemoryIndexFileFactory final : public IndexFileFactory {
  public:
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        return Observation<std::unique_ptr<IndexByteFile>>::available(std::make_unique<MemoryIndexByteFile>());
    }
};

// ---------------------------------------------------------------------------
// Simulated filesystem for budget & rapid cancellation tests
// ---------------------------------------------------------------------------

struct SlowSimControl {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> block_next_open{false};
    bool open_entered = false;
    bool open_released = false;
    std::atomic<unsigned> open_count{0};
    std::atomic<unsigned> delay_open_us{0};

    void wait_entered() {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, kSanitizerAwareTimeout, [&] { return open_entered; }))
            throw std::runtime_error("SlowSimControl::wait_entered timed out");
    }
    void release_open() {
        {
            std::lock_guard lock(mutex);
            open_released = true;
        }
        cv.notify_all();
    }
    void reset_gate() {
        std::lock_guard lock(mutex);
        open_entered = false;
        open_released = false;
    }
};

class BudgetAndStressFS final : public BasicFileSystem {
    ck::testing::SimulatedFileSystem fs_;
    std::shared_ptr<SlowSimControl> ctrl_;

  public:
    explicit BudgetAndStressFS(std::shared_ptr<SlowSimControl> ctrl) : ctrl_(std::move(ctrl)) {
        fs_.bind(take_obs(virtual_address("").host_path(), "root"), fs_.make_node(ObjectKind::Directory));

        auto small_dir = fs_.make_node(ObjectKind::Directory);
        fs_.bind(take_obs(virtual_address("small").host_path(), "small"), small_dir);
        for (int idx = 0; idx < 5; ++idx) {
            fs_.put(small_dir, native_leaf_ascii("small-" + std::to_string(idx) + ".txt"),
                    fs_.make_node(ObjectKind::RegularFile));
        }

        auto small2_dir = fs_.make_node(ObjectKind::Directory);
        fs_.bind(take_obs(virtual_address("small2").host_path(), "small2"), small2_dir);
        for (int idx = 0; idx < 4; ++idx) {
            fs_.put(small2_dir, native_leaf_ascii("recover-" + std::to_string(idx) + ".txt"),
                    fs_.make_node(ObjectKind::RegularFile));
        }

        auto large_dir = fs_.make_node(ObjectKind::Directory);
        fs_.bind(take_obs(virtual_address("large").host_path(), "large"), large_dir);
        const std::string pad(64U, 'x');
        for (int idx = 0; idx < 300; ++idx) {
            fs_.put(large_dir, native_leaf_ascii("entry-" + std::to_string(idx) + "-" + pad + ".dat"),
                    fs_.make_node(ObjectKind::RegularFile));
        }
    }

    Observation<std::unique_ptr<DirectoryCursor>> open_directory(const std::filesystem::path &path,
                                                                 DirectoryLinkPolicy policy) const override {
        if (ctrl_) {
            ++ctrl_->open_count;
            const unsigned delay = ctrl_->delay_open_us.load();
            if (delay > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
            if (ctrl_->block_next_open.exchange(false)) {
                std::unique_lock lock(ctrl_->mutex);
                ctrl_->open_entered = true;
                ctrl_->cv.notify_all();
                ctrl_->cv.wait(lock, [&] { return ctrl_->open_released; });
            }
        }
        return fs_.open_directory(path, policy);
    }

    Observation<BasicMetadata> inspect_basic(const std::filesystem::path &path) const override {
        return fs_.inspect_basic(path);
    }
};

constexpr StorageModel host_storage_model() noexcept {
#if defined(_WIN32)
    return StorageModel::WindowsHandleAttributes;
#else
    return StorageModel::LinuxStatx;
#endif
}

FilesPaneBackend make_stress_backend(std::shared_ptr<SlowSimControl> ctrl) {
    FilesPaneBackend backend;
    backend.filesystem = [ctrl]() -> std::unique_ptr<BasicFileSystem> {
        return std::make_unique<BudgetAndStressFS>(ctrl);
    };
    backend.workspace = []() -> Observation<std::shared_ptr<IndexFileFactory>> {
        return Observation<std::shared_ptr<IndexFileFactory>>::available(std::make_shared<MemoryIndexFileFactory>());
    };
    backend.storage_model = host_storage_model();
    return backend;
}

FilesPaneReply wait_pane_reply(FilesPaneService &service, std::mutex &mtx, std::condition_variable &cv) {
    const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
    for (;;) {
        if (auto reply = service.take())
            return std::move(*reply);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("FilesPaneService timed out waiting for reply");
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------
// Planning fixture helpers
// ---------------------------------------------------------------------------

ObjectInventoryData make_unsupported_object_inventory() {
    ObjectInventoryData result;
    const auto unsupported = []<class T>() {
        return Observation<T>::unavailable(
            ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "wp07.unsupported"});
    };
    result.before = unsupported.template operator()<BasicMetadata>();
    result.after = unsupported.template operator()<BasicMetadata>();
    result.storage = unsupported.template operator()<StorageInfo>();
    result.integrity = unsupported.template operator()<FileIntegrityInfo>();
    result.inode_flags = unsupported.template operator()<FileInodeFlags>();
    result.layout = unsupported.template operator()<FileDataLayout>();
    result.link = unsupported.template operator()<LinkMetadata>();
    result.link_limits = unsupported.template operator()<FileLinkLimits>();
    result.channels = unsupported.template operator()<DataChannelCatalog>();
    result.attributes = unsupported.template operator()<AttributeCatalog>();
    for (auto &sec : result.security)
        sec = unsupported.template operator()<NativeSecurityValue>();
    return result;
}

struct PlanningSimFixture final {
    ck::testing::SimulatedFileSystem fs;
    ck::testing::SimulatedFileSystem::Pointer dst = fs.make_node(ObjectKind::Directory);
    std::shared_ptr<const DirectoryCursor> cursor;

    PlanningSimFixture() {
        fs.bind("/wp07-plan-dst", dst);
        cursor = std::shared_ptr<const DirectoryCursor>(take_obs(fs.open_directory("/wp07-plan-dst"), "plan_dst"));
    }

    FilesOperationPlanningRequest make_request() {
        FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
        const AdapterScope scope;
        auto src = take_obs(NativeAbsolutePath::from_posix("/wp07-src/item.bin"), "plan_src");
        if (!builder.add({std::move(src),
                          {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(42),
                           NativeIdentifier::from_u64(1001), {}},
                          ObjectKind::RegularFile, Observation<std::uint64_t>::available(128), 128,
                          LinkTraversalEvidence::NotLink, {}, {}}) ||
            !builder.seal()) {
            throw std::runtime_error("cannot seal planning fixture inventory");
        }
        auto inv = take_obs(builder.completed(), "plan_inv");

        DestinationBatchJob batch_job(cursor, {*NativeName::from_posix("item.bin")}, NameEncoding::PosixBytes, {},
                                      fs.clock());
        std::shared_ptr<const CompletedDestinationBatch> batch;
        for (int step = 0; step < 200; ++step) {
            if (batch_job.advance() == DestinationBatchState::Complete) {
                batch = take_obs(batch_job.completed(), "plan_batch");
                break;
            }
        }
        if (!batch)
            throw std::runtime_error("destination batch job did not complete");

        BasicMetadata src_meta;
        src_meta.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        FilesOperationPlanningRequest req;
        req.operation = FileOperation::Copy;
        req.inventory = std::move(inv);
        req.source_target = {SourceTargetInspection{
            Observation<BasicMetadata>::available(src_meta), {},
            Observation<IdentityComparison>::unavailable(
                ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable),
                 "source_target.no_existing_target"})}};
        req.ancestry = {Observation<AncestorWalkWitness>::unavailable(
            ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "wp07.no_ancestry"})};
        req.source_objects = {make_unsupported_object_inventory()};
        req.destination_batch = std::move(batch);
        req.destination_capabilities = {DestinationCapabilityEvidence{
            VolumeFeature::SparseFiles,
            Observation<FeatureAdvertisement>::available(FeatureAdvertisement::Advertised)}};
        return req;
    }
};

// ---------------------------------------------------------------------------
// Crash-child protocol (self-reexec helper + parent process spawner/killer)
// ---------------------------------------------------------------------------

constexpr std::string_view kOriginalSourceBytes = "ORIGINAL-SOURCE-BYTES-WP07-PAYLOAD-0123456789";
constexpr std::string_view kPriorDestinationBytes = "PRIOR-DESTINATION-BYTES-MUST-STAY-100%-INTACT";
constexpr std::string_view kDisjointSourceBytes = "DISJOINT-WORKSPACE-SOURCE-BYTES";
constexpr std::string_view kStagedPartialBytes = "PARTIAL-UNCOMMITTED-STAGING-BYTES";

OperationJournalId crash_journal_id(std::uint8_t tag) {
    OperationJournalId id{};
    id.bytes[0] = std::byte{0xC7};
    id.bytes[1] = std::byte{tag};
    id.bytes[2] = std::byte{0x07};
    return id;
}

int run_crash_child(std::string_view mode, const std::filesystem::path &root_dir) {
    try {
        if (mode == "sync_session_lock") {
            const auto coord_addr = host_address(root_dir / "coord");
            const auto left_addr = host_address(root_dir / "left");
            const auto right_addr = host_address(root_dir / "right");
            auto backend = take_obs(open_native_sync_session_backend(coord_addr, left_addr, right_addr),
                                    "child.sync_session_lock");
            std::fputs("READY\n", stdout);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(15));
            (void)backend->finish_io();
            return 99;
        }
        const auto src_a = root_dir / "src-a";
        const auto dst_a = root_dir / "dst-a";
        const auto journals = root_dir / "journals";

        const auto journals_addr = host_address(journals);
        const auto dst_a_addr = host_address(dst_a);

        NativeFileSystem fs;
        auto dir_src_a = take_obs(fs.open_native_directory(host_address(src_a)), "child.open_src_a");
        auto dir_dst_a = take_obs(fs.open_native_directory(dst_a_addr), "child.open_dst_a");

        const auto payload_name = native_leaf("payload.bin", u"payload.bin");
        auto src_entry = take_obs(dir_src_a->open_child(payload_name, EntryAccess::MetadataOnly), "child.open_src_file");
        const auto src_meta = take_obs(src_entry->inspect_basic(), "child.src_meta");
        const auto id_src_a = take_obs(dir_src_a->identity(), "child.id_src_a");
        const auto id_dst_a = take_obs(dir_dst_a->identity(), "child.id_dst_a");
        const auto id_payload = take_obs(src_meta.identity, "child.id_payload");

        const bool is_move = (mode == "journal_move");
        const auto staging_name = is_move ? native_leaf(".ck-move-staging-crash", u".ck-move-staging-crash")
                                          : native_leaf(".ck-copy-staging-crash", u".ck-copy-staging-crash");
        const auto journal_name = is_move ? native_leaf(".ck-move-journal-wp07-crash", u".ck-move-journal-wp07-crash")
                                          : native_leaf(".ck-copy-journal-wp07-crash", u".ck-copy-journal-wp07-crash");

        auto staging_entry = take_obs(dir_dst_a->create_child_file(staging_name), "child.create_staging");
        const auto staging_meta = take_obs(staging_entry->inspect_basic(), "child.staging_meta");
        const auto id_staging = take_obs(staging_meta.identity, "child.id_staging");
        // Write partial staging bytes so the staging artifact is non-empty on disk.
        // Release the metadata handle first on Windows so exclusive write succeeds, then reopen for holding.
        staging_entry.reset();
        const auto staging_path = dst_a / (is_move ? ".ck-move-staging-crash" : ".ck-copy-staging-crash");
        if (!write_native_file_bytes(staging_path, kStagedPartialBytes))
            return 21;
        staging_entry = take_obs(dir_dst_a->open_child(staging_name, EntryAccess::MetadataOnly), "child.reopen_staging");

        const auto dur_src_parent = NativeDurableIdentity::from_live(id_src_a);
        const auto dur_src_obj = NativeDurableIdentity::from_live(id_payload);
        const auto dur_dst_dir = NativeDurableIdentity::from_live(id_dst_a);
        const auto dur_staging = NativeDurableIdentity::from_live(id_staging);
        if (!dur_src_parent || !dur_src_obj || !dur_dst_dir || !dur_staging)
            return 22;

        NativeOperationRecoveryEvidence ev{
            staging_name,
            payload_name,
            *dur_src_parent,
            *dur_src_obj,
            *dur_dst_dir,
            *dur_staging,
            dst_a_addr,
            std::nullopt,
            std::nullopt,
            StagingNamespace::Named,
            payload_name,
        };
        const auto encoded = take_obs(encode_native_operation_recovery_evidence(ev), "child.encode_ev");

        const auto op = is_move ? FileOperation::Move : FileOperation::Copy;
        const auto jid = crash_journal_id(is_move ? 0x22U : 0x11U);
        auto journal = take_obs(OperationJournal::create(op, jid, {4, 32, 4096}), "child.create_journal");
        auto jfile = take_obs(open_native_operation_journal_file(journals_addr, journal_name, true), "child.open_jfile");

        std::uint64_t seq = 1;
        const auto append_state = [&](OperationJournalState st, bool with_payload) {
            const std::vector<std::byte> payload = with_payload ? encoded : std::vector<std::byte>{};
            (void)take_obs(append_operation_journal_file(*jfile, *journal, {seq++, 0, st, payload}), "child.append");
        };

        append_state(OperationJournalState::Planned, false);
        append_state(OperationJournalState::Prepared, true);
        append_state(OperationJournalState::Copied, true);
        if (is_move) {
            append_state(OperationJournalState::Verified, true);
            append_state(OperationJournalState::CommitIntended, true);
            append_state(OperationJournalState::DestinationCommitted, true);
        }
        // Flush the journal durably to disk, but keep jfile & staging handles open
        // without calling finish_io() or publishing — simulating an abrupt mid-operation crash.
        (void)take_obs(jfile->flush(), "child.flush");

        std::fputs("READY\n", stdout);
        std::fflush(stdout);

        // Hold handles open while waiting for parent's SIGKILL / TerminateProcess.
        // Safety cap prevents an orphaned process from lingering if parent ever died.
        std::this_thread::sleep_for(std::chrono::seconds(15));
        return 99;
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "crash child error: %s\n", ex.what());
        std::fflush(stderr);
        return 23;
    }
}

#if defined(_WIN32)
std::wstring quote_win_arg(const std::wstring &arg) {
    std::wstring out;
    out.push_back(L'\"');
    std::size_t slashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"')
            out.append(slashes * 2U + 1U, L'\\');
        else
            out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(ch);
    }
    out.append(slashes * 2U, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring current_executable_wide() {
    std::wstring buf(512U, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            throw std::runtime_error("GetModuleFileNameW failed");
        if (n < buf.size() - 1U) {
            buf.resize(static_cast<std::size_t>(n));
            return buf;
        }
        buf.resize(buf.size() * 2U);
    }
}

struct ScopedWinEnvVar {
    std::wstring name;
    std::optional<std::wstring> prior;
    ScopedWinEnvVar(std::wstring var_name, const std::wstring &value) : name(std::move(var_name)) {
        const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring old(needed, L'\0');
            const DWORD got = GetEnvironmentVariableW(name.c_str(), old.data(), needed);
            if (got > 0 && got < needed) {
                old.resize(got);
                prior = std::move(old);
            }
        }
        SetEnvironmentVariableW(name.c_str(), value.c_str());
    }
    ~ScopedWinEnvVar() {
        SetEnvironmentVariableW(name.c_str(), prior ? prior->c_str() : nullptr);
    }
    ScopedWinEnvVar(const ScopedWinEnvVar &) = delete;
    ScopedWinEnvVar &operator=(const ScopedWinEnvVar &) = delete;
};

bool spawn_and_crash_child(std::string_view mode, const std::filesystem::path &root_dir,
                           const std::function<void()> &on_ready = {}) {
    const std::wstring exe_raw = current_executable_wide();
    const auto exe_ext = windows_extended_path(exe_raw);
    if (!exe_ext)
        return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return false;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring mode_w;
    for (const char ch : mode)
        mode_w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));

    PROCESS_INFORMATION pi{};
    bool created = false;
    {
        ScopedWinEnvVar env_mode(L"CKFILES_WP07_CRASH_CHILD", mode_w);
        ScopedWinEnvVar env_root(L"CKFILES_WP07_CRASH_ROOT", root_dir.native());
        ScopedWinEnvVar env_asan(L"ASAN_OPTIONS", L"detect_leaks=0:abort_on_error=0");

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        std::wstring cmd = quote_win_arg(*exe_ext);
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');

        created = CreateProcessW(exe_ext->c_str(), cmd_buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                 &si, &pi) != 0;
    }
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return false;
    }

    std::string ready_buf;
    char ch = '\0';
    DWORD got = 0;
    const auto has_ready_line = [&] {
        return ready_buf.find("READY\n") != std::string::npos || ready_buf.find("READY\r\n") != std::string::npos;
    };
    while (!has_ready_line()) {
        if (!ReadFile(read_pipe, &ch, 1, &got, nullptr) || got == 0)
            break;
        ready_buf.push_back(ch);
        if (ready_buf.size() > 256U)
            break;
    }
    CloseHandle(read_pipe);

    const bool ready_seen = has_ready_line();
    if (ready_seen && on_ready)
        on_ready();
    constexpr UINT kKillCode = 137U;
    const bool terminated = ready_seen && TerminateProcess(pi.hProcess, kKillCode) != 0;
    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, 10000U);
    DWORD exit_code = 0;
    const bool got_code = (wait_rc == WAIT_OBJECT_0) && (GetExitCodeProcess(pi.hProcess, &exit_code) != 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ready_seen && terminated && got_code && exit_code == kKillCode;
}
#else
std::string current_executable_posix() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::string buf(size, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) == 0) {
            const auto nul = buf.find('\0');
            if (nul != std::string::npos)
                buf.resize(nul);
            return buf;
        }
    }
#elif defined(__linux__)
    std::string buf(4096U, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1U);
    if (n > 0) {
        buf.resize(static_cast<std::size_t>(n));
        return buf;
    }
#endif
    return g_argv0;
}

struct ScopedPosixEnvVar {
    std::string name;
    std::optional<std::string> prior;
    ScopedPosixEnvVar(std::string var_name, const std::string &value) : name(std::move(var_name)) {
        if (const char *old = std::getenv(name.c_str()))
            prior = std::string(old);
        ::setenv(name.c_str(), value.c_str(), 1);
    }
    ~ScopedPosixEnvVar() {
        if (prior)
            ::setenv(name.c_str(), prior->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
    ScopedPosixEnvVar(const ScopedPosixEnvVar &) = delete;
    ScopedPosixEnvVar &operator=(const ScopedPosixEnvVar &) = delete;
};

bool spawn_and_crash_child(std::string_view mode, const std::filesystem::path &root_dir,
                           const std::function<void()> &on_ready = {}) {
    const std::string exe = current_executable_posix();
    if (exe.empty())
        return false;
    int pipefd[2]{-1, -1};
    if (::pipe(pipefd) != 0)
        return false;

    pid_t pid = -1;
    {
        ScopedPosixEnvVar env_mode("CKFILES_WP07_CRASH_CHILD", std::string(mode));
        ScopedPosixEnvVar env_root("CKFILES_WP07_CRASH_ROOT", root_dir.string());
        ScopedPosixEnvVar env_asan("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0");
        ScopedPosixEnvVar env_lsan("LSAN_OPTIONS", "detect_leaks=0");

        pid = ::fork();
        if (pid == 0) {
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            char *const argv_child[] = {const_cast<char *>(exe.c_str()), nullptr};
            ::execv(exe.c_str(), argv_child);
            ::_exit(127);
        }
    }
    ::close(pipefd[1]);
    if (pid < 0) {
        ::close(pipefd[0]);
        return false;
    }

    std::string ready_buf;
    const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
    while (ready_buf.find("READY\n") == std::string::npos) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd{};
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        const int prc = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(rem_ms, 500)));
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (prc == 0)
            continue;
        char ch = '\0';
        const ssize_t n = ::read(pipefd[0], &ch, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        ready_buf.push_back(ch);
        if (ready_buf.size() > 256U)
            break;
    }
    ::close(pipefd[0]);

    const bool ready_seen = ready_buf.find("READY\n") != std::string::npos;
    if (ready_seen && on_ready)
        on_ready();
    const bool killed = (::kill(pid, SIGKILL) == 0);
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    return ready_seen && killed && waited == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}
#endif

void verify_real_process_crash_recovery(std::string_view mode, std::uint8_t expected_tag) {
    const ck::testing::ScratchDirectory scratch(
        mode == "journal_move" ? "wp07-crash-move" : "wp07-crash-copy",
        CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);

    const auto src_a = scratch.path() / "src-a";
    const auto dst_a = scratch.path() / "dst-a";
    const auto src_b = scratch.path() / "src-b";
    const auto dst_b = scratch.path() / "dst-b";
    const auto journals = scratch.path() / "journals";
    const auto idx_src = scratch.path() / "idx-src";
    const auto idx_dst = scratch.path() / "idx-dst";
    for (const auto &dir : {src_a, dst_a, src_b, dst_b, journals, idx_src, idx_dst})
        CK_CHECK(create_private_dir(dir));

    CK_CHECK(write_native_file_bytes(src_a / "payload.bin", kOriginalSourceBytes));
    CK_CHECK(write_native_file_bytes(dst_a / "payload.bin", kPriorDestinationBytes));
    CK_CHECK(write_native_file_bytes(src_b / "disjoint.bin", kDisjointSourceBytes));

    const auto journals_addr = host_address(journals);
    CK_CHECK(prepare_native_operation_journal_root(journals_addr).value());

    // Spawn self-reexec child, wait for READY after staging + flushed journal, then SIGKILL / TerminateProcess.
    CK_CHECK(spawn_and_crash_child(mode, scratch.path()));

    // (a) Original source bytes & destination prior bytes must be 100% intact.
    std::string src_after;
    std::string dst_after;
    CK_CHECK(read_native_file_bytes(src_a / "payload.bin", src_after));
    CK_CHECK(read_native_file_bytes(dst_a / "payload.bin", dst_after));
    CK_CHECK(src_after == kOriginalSourceBytes);
    CK_CHECK(dst_after == kPriorDestinationBytes);

    // (b) Restart recovery scan detects the crash-interrupted journal by native identity.
    const auto scan = discover_operation_recovery(journals_addr);
    CK_CHECK(scan.value() && scan.value()->journals.size() == 1U && scan.value()->requires_reconciliation);
    if (!scan.value() || scan.value()->journals.size() != 1U)
        return;

    const auto &item = scan.value()->journals.front();
    CK_CHECK(item.report.has_value());
    if (item.report) {
        CK_CHECK(item.report->journal_id == crash_journal_id(expected_tag));
        CK_CHECK(item.report->inspection.report.requires_reconciliation);
    }

    // Open live native identities for both overlapping (src-a/payload.bin -> dst-a) and disjoint (src-b -> dst-b).
    NativeFileSystem fs;
    auto dir_src_a = take_obs(fs.open_native_directory(host_address(src_a)), "parent.src_a");
    auto dir_dst_a = take_obs(fs.open_native_directory(host_address(dst_a)), "parent.dst_a");
    auto dir_src_b = take_obs(fs.open_native_directory(host_address(src_b)), "parent.src_b");
    auto dir_dst_b = take_obs(fs.open_native_directory(host_address(dst_b)), "parent.dst_b");

    auto file_a = take_obs(dir_src_a->open_child(native_leaf("payload.bin", u"payload.bin"), EntryAccess::MetadataOnly),
                           "parent.file_a");
    auto file_b = take_obs(dir_src_b->open_child(native_leaf("disjoint.bin", u"disjoint.bin"), EntryAccess::MetadataOnly),
                           "parent.file_b");

    const auto id_src_a = take_obs(dir_src_a->identity(), "id_src_a");
    const auto id_dst_a = take_obs(dir_dst_a->identity(), "id_dst_a");
    const auto id_src_b = take_obs(dir_src_b->identity(), "id_src_b");
    const auto id_dst_b = take_obs(dir_dst_b->identity(), "id_dst_b");
    const auto id_file_a = take_obs(take_obs(file_a->inspect_basic(), "meta_a").identity, "id_file_a");
    const auto id_file_b = take_obs(take_obs(file_b->inspect_basic(), "meta_b").identity, "id_file_b");

    auto make_inv = [](const std::filesystem::path &p, const ObjectIdentity &id, std::uint64_t bytes) {
        FrozenInventoryBuilder builder(InventoryMountPolicy::DirectObjects, InventoryLinkPolicy::PreserveLinks);
        FrozenInventoryEntry entry{host_address(p), id, ObjectKind::RegularFile, {}, bytes,
                                   LinkTraversalEvidence::NotLink, std::nullopt, {}};
        CK_CHECK(builder.add(std::move(entry)));
        CK_CHECK(builder.seal());
        return take_obs(builder.completed(), "crash.inv");
    };

    const auto inv_a = make_inv(src_a / "payload.bin", id_file_a, kOriginalSourceBytes.size());
    const auto inv_b = make_inv(src_b / "disjoint.bin", id_file_b, kDisjointSourceBytes.size());

    const auto overlap_a = assess_operation_recovery_overlap(*scan.value(), *inv_a, id_dst_a, id_src_a);
    CK_CHECK(overlap_a.has_conflict());
    if (!overlap_a.conflicts.empty())
        CK_CHECK(overlap_a.conflicts.front().kind == FilesOperationRecoveryOverlapKind::UnfinishedTransfer);

    // (c) Disjoint transfer in the same workspace remains unblocked.
    const auto overlap_b = assess_operation_recovery_overlap(*scan.value(), *inv_b, id_dst_b, id_src_b);
    CK_CHECK(!overlap_b.has_conflict());

    // End-to-end FilesOperationPlanningService check: repeat submission fails closed with ReconciliationRequired,
    // while disjoint submission succeeds with Ready.
    std::mutex src_mtx;
    std::condition_variable src_cv;
    std::mutex dst_mtx;
    std::condition_variable dst_cv;
    std::mutex plan_mtx;
    std::condition_variable plan_cv;

    FilesPaneService src_pane(native_files_pane_backend(host_address(idx_src)), {}, [&] { src_cv.notify_all(); });
    FilesPaneService dst_pane(native_files_pane_backend(host_address(idx_dst)), {}, [&] { dst_cv.notify_all(); });
    FilesOperationPlanningService planning([&] { plan_cv.notify_all(); });

    struct ScopedStop {
        FilesOperationPlanningService &p;
        FilesPaneService &s;
        FilesPaneService &d;
        ~ScopedStop() {
            p.request_stop();
            p.join();
            s.request_stop();
            d.request_stop();
            s.join();
            d.join();
        }
    } stop_guard{planning, src_pane, dst_pane};

    const auto plan_wait = [&]() -> FilesOperationPlanningReply {
        const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
        for (;;) {
            if (auto r = planning.take())
                return std::move(*r);
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("planning service timed out");
            std::unique_lock lock(plan_mtx);
            plan_cv.wait_for(lock, std::chrono::milliseconds(5));
        }
    };

    // Navigate to src-a & dst-a, mark payload.bin, submit repeat transfer -> ReconciliationRequired.
    const auto nav_sa = src_pane.navigate(host_address(src_a));
    const auto nav_da = dst_pane.navigate(host_address(dst_a));
    CK_CHECK(nav_sa.state == FilesPaneSubmission::Accepted && nav_da.state == FilesPaneSubmission::Accepted);
    const auto rep_sa = wait_pane_reply(src_pane, src_mtx, src_cv);
    const auto rep_da = wait_pane_reply(dst_pane, dst_mtx, dst_cv);
    CK_CHECK(rep_sa.state == FilesPaneReplyState::Ready && rep_da.state == FilesPaneReplyState::Ready);

    const auto mark_sa = src_pane.change(nav_sa.ticket, rep_sa.stamp,
                                         FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 10});
    CK_CHECK(mark_sa.state == FilesPaneSubmission::Accepted);
    const auto marked_sa = wait_pane_reply(src_pane, src_mtx, src_cv);
    CK_CHECK(marked_sa.state == FilesPaneReplyState::Ready);

    FilesOperationPanePlanningRequest repeat_req;
    repeat_req.operation = (mode == "journal_move") ? FileOperation::Move : FileOperation::Copy;
    repeat_req.source_service = &src_pane;
    repeat_req.destination_service = &dst_pane;
    repeat_req.source_navigation = nav_sa.ticket;
    repeat_req.destination_navigation = nav_da.ticket;
    repeat_req.source_stamp = marked_sa.stamp;
    repeat_req.destination_stamp = rep_da.stamp;
    repeat_req.source_limits.access = EntryAccess::MetadataOnly;
    repeat_req.evidence_limits.max_items = repeat_req.source_limits.held_entries;
    repeat_req.journal_root = journals_addr;

    CK_CHECK(planning.request(repeat_req).state == FilesOperationPlanningSubmission::Accepted);
    const auto repeat_reply = plan_wait();
    CK_CHECK(repeat_reply.state == FilesOperationPlanningReplyState::ReconciliationRequired);
    CK_CHECK(repeat_reply.reconciliation && repeat_reply.reconciliation->has_conflict());

    // Now navigate to disjoint src-b & dst-b -> Ready (unblocked).
    const auto nav_sb = src_pane.navigate(host_address(src_b));
    const auto nav_db = dst_pane.navigate(host_address(dst_b));
    CK_CHECK(nav_sb.state == FilesPaneSubmission::Accepted && nav_db.state == FilesPaneSubmission::Accepted);
    const auto rep_sb = wait_pane_reply(src_pane, src_mtx, src_cv);
    const auto rep_db = wait_pane_reply(dst_pane, dst_mtx, dst_cv);
    CK_CHECK(rep_sb.state == FilesPaneReplyState::Ready && rep_db.state == FilesPaneReplyState::Ready);

    const auto mark_sb = src_pane.change(nav_sb.ticket, rep_sb.stamp,
                                         FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 10});
    CK_CHECK(mark_sb.state == FilesPaneSubmission::Accepted);
    const auto marked_sb = wait_pane_reply(src_pane, src_mtx, src_cv);
    CK_CHECK(marked_sb.state == FilesPaneReplyState::Ready);

    FilesOperationPanePlanningRequest disjoint_req;
    disjoint_req.operation = FileOperation::Copy;
    disjoint_req.source_service = &src_pane;
    disjoint_req.destination_service = &dst_pane;
    disjoint_req.source_navigation = nav_sb.ticket;
    disjoint_req.destination_navigation = nav_db.ticket;
    disjoint_req.source_stamp = marked_sb.stamp;
    disjoint_req.destination_stamp = rep_db.stamp;
    disjoint_req.source_limits.access = EntryAccess::MetadataOnly;
    disjoint_req.evidence_limits.max_items = disjoint_req.source_limits.held_entries;
    disjoint_req.journal_root = journals_addr;

    CK_CHECK(planning.request(disjoint_req).state == FilesOperationPlanningSubmission::Accepted);
    const auto disjoint_reply = plan_wait();
    CK_CHECK(disjoint_reply.state == FilesOperationPlanningReplyState::Ready);
}

} // namespace

// ===========================================================================
// 1. Index storage budget exhaustion & recovery
// ===========================================================================

CK_TEST(wp07_index_storage_budget_exhaustion_fails_listing_and_recovers) {
    auto ctrl = std::make_shared<SlowSimControl>();
    BudgetAndStressFS fs(ctrl);
    auto factory = std::make_shared<MemoryIndexFileFactory>();

    // Byte-budget exhaustion: small succeeds, large fails cleanly, budget returns to 0, subsequent small succeeds.
    auto byte_budget = std::make_shared<IndexStorageBudget>(4096U, 4U);
    auto run_listing = [&](const char *dir_name, std::uint8_t gen_byte,
                           const std::shared_ptr<IndexStorageBudget> &budget,
                           DirectoryIndexStoreLimits store_limits) {
        auto cursor = take_obs(
            fs.open_directory(take_obs(virtual_address(dir_name).host_path(), "path"),
                              DirectoryLinkPolicy::RejectFinalLink),
            "open");
        PaneListingRequest req{AdapterScope{}, gen_byte};
        auto listing = std::make_unique<DirectoryListing>(std::move(cursor), DirectoryListingLimits{},
                                                          system_monotonic_clock());
        std::array<std::byte, 16> gen{};
        gen[0] = std::byte{gen_byte};
        DirectoryIndexListingJob job(req, std::move(listing),
                                     host_storage_model(), factory, budget, gen, store_limits);
        for (int step = 0; step < 5000; ++step) {
            const auto st = job.advance();
            if (st == DirectoryIndexListingState::Complete || st == DirectoryIndexListingState::Failed ||
                st == DirectoryIndexListingState::Cancelled)
                break;
        }
        return std::pair{job.state(), job.completed()};
    };

    DirectoryIndexStoreLimits store_limits{};
    store_limits.bytes = 4096U;
    store_limits.rows = 10000U;

    {
        auto [st, result] = run_listing("small", 1U, byte_budget, store_limits);
        CK_CHECK(st == DirectoryIndexListingState::Complete && result.value() != nullptr);
        CK_CHECK(byte_budget->charged_files() == 1U && byte_budget->charged_bytes() > 0U);
    }
    CK_CHECK(byte_budget->charged_files() == 0U && byte_budget->charged_bytes() == 0U);

    {
        auto [st, result] = run_listing("large", 2U, byte_budget, store_limits);
        CK_CHECK(st == DirectoryIndexListingState::Failed && !result.value() && result.unavailable_detail() != nullptr);
    }
    CK_CHECK(byte_budget->charged_files() == 0U && byte_budget->charged_bytes() == 0U);

    {
        auto [st, result] = run_listing("small2", 3U, byte_budget, store_limits);
        CK_CHECK(st == DirectoryIndexListingState::Complete && result.value() != nullptr);
    }
    CK_CHECK(byte_budget->charged_files() == 0U && byte_budget->charged_bytes() == 0U);

    // File-slot exhaustion: max_files = 2 allows one listing to sort (using 2 slots) and retain 1 slot,
    // then blocks a concurrent second listing (needing 2 more slots) until the first is released.
    auto slot_budget = std::make_shared<IndexStorageBudget>(64U * 1024U, 2U);
    DirectoryIndexStoreLimits wide_limits{};
    wide_limits.bytes = 64U * 1024U;
    wide_limits.rows = 10000U;
    {
        auto [st1, res1] = run_listing("small", 4U, slot_budget, wide_limits);
        CK_CHECK(st1 == DirectoryIndexListingState::Complete && res1.value() != nullptr);
        CK_CHECK(slot_budget->charged_files() == 1U);

        auto [st2, res2] = run_listing("small2", 5U, slot_budget, wide_limits);
        CK_CHECK(st2 == DirectoryIndexListingState::Failed && !res2.value());
        CK_CHECK(slot_budget->charged_files() == 1U);
    }
    CK_CHECK(slot_budget->charged_files() == 0U && slot_budget->charged_bytes() == 0U);

    {
        auto [st3, res3] = run_listing("small2", 6U, slot_budget, wide_limits);
        CK_CHECK(st3 == DirectoryIndexListingState::Complete && res3.value() != nullptr);
    }
    CK_CHECK(slot_budget->charged_files() == 0U && slot_budget->charged_bytes() == 0U);
}

CK_TEST(wp07_pane_service_index_budget_exhaustion_preserves_prior_pane_state) {
    auto ctrl = std::make_shared<SlowSimControl>();
    std::mutex mtx;
    std::condition_variable cv;

    FilesPaneServiceLimits limits{};
    limits.cache_bytes = 16384U;
    limits.cache_files = 8U;

    FilesPaneService service(make_stress_backend(ctrl), limits, [&] { cv.notify_all(); });
    const auto small_addr = virtual_address("small");
    const auto large_addr = virtual_address("large");
    const auto small2_addr = virtual_address("small2");

    // Initial navigation to /small succeeds.
    const auto sub1 = service.navigate(small_addr, {}, 64U);
    CK_CHECK(sub1.state == FilesPaneSubmission::Accepted);
    const auto rep1 = wait_pane_reply(service, mtx, cv);
    CK_CHECK(rep1.state == FilesPaneReplyState::Ready);
    CK_CHECK(rep1.page && rep1.page->rows().size() == 5U);
    CK_CHECK(rep1.active_location && rep1.active_location->root_address() == small_addr);

    // Navigation to /large exceeds cache_bytes -> fails cleanly without corrupting prior published pane.
    const auto sub2 = service.navigate(large_addr, {}, 64U);
    CK_CHECK(sub2.state == FilesPaneSubmission::Accepted);
    const auto rep2 = wait_pane_reply(service, mtx, cv);
    CK_CHECK(rep2.state == FilesPaneReplyState::Failed || rep2.state == FilesPaneReplyState::LimitExceeded);
    CK_CHECK(rep2.active_location && rep2.active_location->root_address() == small_addr);
    CK_CHECK(service.running() && service.fault() == FilesPaneWorkerFault::None);

    // Prior published pane is still readable with its original stamp under the current navigation epoch.
    const auto page_sub = service.read_page(rep2.navigation, rep1.stamp, {0U, 10U});
    CK_CHECK(page_sub.state == FilesPaneSubmission::Accepted);
    if (page_sub.state == FilesPaneSubmission::Accepted) {
        const auto page_rep = wait_pane_reply(service, mtx, cv);
        CK_CHECK(page_rep.state == FilesPaneReplyState::Ready);
        CK_CHECK(page_rep.page && page_rep.page->rows().size() == 5U);
    }

    // Subsequent navigation within budget (/small2) succeeds.
    const auto sub3 = service.navigate(small2_addr, {}, 64U);
    CK_CHECK(sub3.state == FilesPaneSubmission::Accepted);
    const auto rep3 = wait_pane_reply(service, mtx, cv);
    CK_CHECK(rep3.state == FilesPaneReplyState::Ready);
    CK_CHECK(rep3.page && rep3.page->rows().size() == 4U);
    CK_CHECK(rep3.active_location && rep3.active_location->root_address() == small2_addr);

    service.request_stop();
    service.join();
}

// ===========================================================================
// 2. Repeated rapid cancellation & service lifetime stress
// ===========================================================================

CK_TEST(wp07_pane_service_rapid_cancel_restart_and_in_flight_destruction) {
    // Phase A: 30 rapid cancel + supersede + restart cycles.
    {
        auto ctrl = std::make_shared<SlowSimControl>();
        ctrl->delay_open_us.store(100U);
        std::mutex mtx;
        std::condition_variable cv;
        FilesPaneService service(make_stress_backend(ctrl), {}, [&] { cv.notify_all(); });

        const auto small_addr = virtual_address("small");
        const auto small2_addr = virtual_address("small2");
        for (int iter = 0; iter < 30; ++iter) {
            const auto &target = (iter % 2 == 0) ? small_addr : small2_addr;
            const auto sub = service.navigate(target, {}, 32U);
            CK_CHECK(sub.state == FilesPaneSubmission::Accepted);
            if (iter % 3 != 0)
                (void)service.cancel(sub.ticket);
            (void)service.take();
        }

        // Final uncancelled navigation must succeed cleanly with zero faults.
        ctrl->delay_open_us.store(0U);
        const auto final_sub = service.navigate(small_addr, {}, 32U);
        CK_CHECK(final_sub.state == FilesPaneSubmission::Accepted);
        FilesPaneReply final_rep;
        const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
        bool got_final = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto r = service.take()) {
                if (r->request == final_sub.ticket) {
                    final_rep = std::move(*r);
                    got_final = true;
                    break;
                }
            } else {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
        CK_CHECK(got_final && final_rep.state == FilesPaneReplyState::Ready);
        CK_CHECK(final_rep.page && final_rep.page->rows().size() == 5U);
        CK_CHECK(service.running() && service.fault() == FilesPaneWorkerFault::None);
        service.request_stop();
        service.join();
    }

    // Phase B: Destroy FilesPaneService while navigation is actively in-flight; no late wake callbacks after dtor.
    for (int round = 0; round < 6; ++round) {
        auto ctrl = std::make_shared<SlowSimControl>();
        ctrl->block_next_open.store(true);
        auto owner_destroyed = std::make_shared<std::atomic<bool>>(false);
        auto late_wakes = std::make_shared<std::atomic<unsigned>>(0U);

        auto service = std::make_unique<FilesPaneService>(make_stress_backend(ctrl), FilesPaneServiceLimits{},
                                                          [owner_destroyed, late_wakes] {
                                                              if (owner_destroyed->load(std::memory_order_acquire))
                                                                  late_wakes->fetch_add(1U, std::memory_order_relaxed);
                                                          });
        const auto sub = service->navigate(virtual_address("large"), {}, 64U);
        CK_CHECK(sub.state == FilesPaneSubmission::Accepted);
        ctrl->wait_entered();

        // Release the blocked open concurrently while destroying the service.
        std::thread releaser([ctrl] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ctrl->release_open();
        });
        service.reset();
        owner_destroyed->store(true, std::memory_order_release);
        releaser.join();
        CK_CHECK(late_wakes->load(std::memory_order_acquire) == 0U);
    }
}

CK_TEST(wp07_operation_planning_service_rapid_cancel_and_in_flight_destruction) {
    PlanningSimFixture fixture;

    // Phase A: 30 rapid submit + cancel cycles.
    {
        std::mutex mtx;
        std::condition_variable cv;
        FilesOperationPlanningService service([&] { cv.notify_all(); });

        for (int iter = 0; iter < 30; ++iter) {
            auto sub = service.request(fixture.make_request());
            if (sub.state == FilesOperationPlanningSubmission::Accepted) {
                (void)service.cancel(sub.ticket);
            }
            // Drain any completed/cancelled reply so the next iteration can submit cleanly.
            const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
            while (service.working() && std::chrono::steady_clock::now() < deadline) {
                if (service.take())
                    break;
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(2));
            }
            (void)service.take();
        }

        // Drain any remaining reply before submitting the final uncancelled request.
        {
            const auto drain_deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
            while (service.working() && std::chrono::steady_clock::now() < drain_deadline) {
                (void)service.take();
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(2));
            }
            (void)service.take();
        }

        const auto final_sub = service.request(fixture.make_request());
        CK_CHECK(final_sub.state == FilesOperationPlanningSubmission::Accepted);
        std::optional<FilesOperationPlanningReply> reply;
        const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
        while (!reply && std::chrono::steady_clock::now() < deadline) {
            if (auto r = service.take()) {
                if (r->request == final_sub.ticket)
                    reply = std::move(*r);
            } else {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
        CK_CHECK(reply && reply->state == FilesOperationPlanningReplyState::Ready);
        CK_CHECK(service.running() && service.fault() == FilesPaneWorkerFault::None);
        service.request_stop();
        service.join();
    }

    // Phase B: Destroy FilesOperationPlanningService while requests are in-flight; 0 late wakes.
    for (int round = 0; round < 8; ++round) {
        auto owner_destroyed = std::make_shared<std::atomic<bool>>(false);
        auto late_wakes = std::make_shared<std::atomic<unsigned>>(0U);
        auto service = std::make_unique<FilesOperationPlanningService>(
            [owner_destroyed, late_wakes] {
                if (owner_destroyed->load(std::memory_order_acquire))
                    late_wakes->fetch_add(1U, std::memory_order_relaxed);
            });
        const auto sub = service->request(fixture.make_request());
        CK_CHECK(sub.state == FilesOperationPlanningSubmission::Accepted);
        service.reset();
        owner_destroyed->store(true, std::memory_order_release);
        CK_CHECK(late_wakes->load(std::memory_order_acquire) == 0U);
    }
}

CK_TEST(wp07_operation_execution_service_rapid_cancel_and_in_flight_destruction) {
    const ck::testing::ScratchDirectory scratch("wp07-exec-cancel", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto src_dir = scratch.path() / "src";
    const auto dst_dir = scratch.path() / "dst";
    const auto idx_src = scratch.path() / "idx-src";
    const auto idx_dst = scratch.path() / "idx-dst";
    const auto journals = scratch.path() / "journals";
    for (const auto &d : {src_dir, dst_dir, idx_src, idx_dst, journals})
        CK_CHECK(create_private_dir(d));
    CK_CHECK(write_native_file_bytes(src_dir / "item.bin", "EXECUTION-CANCEL-STRESS-DATA"));

    std::mutex src_mtx;
    std::condition_variable src_cv;
    std::mutex dst_mtx;
    std::condition_variable dst_cv;
    FilesPaneService src_pane(native_files_pane_backend(host_address(idx_src)), {}, [&] { src_cv.notify_all(); });
    FilesPaneService dst_pane(native_files_pane_backend(host_address(idx_dst)), {}, [&] { dst_cv.notify_all(); });

    struct StopPanes {
        FilesPaneService &s;
        FilesPaneService &d;
        ~StopPanes() {
            s.request_stop();
            d.request_stop();
            s.join();
            d.join();
        }
    } stop_panes{src_pane, dst_pane};

    const auto nav_s = src_pane.navigate(host_address(src_dir));
    const auto nav_d = dst_pane.navigate(host_address(dst_dir));
    CK_CHECK(nav_s.state == FilesPaneSubmission::Accepted && nav_d.state == FilesPaneSubmission::Accepted);
    const auto rep_s = wait_pane_reply(src_pane, src_mtx, src_cv);
    const auto rep_d = wait_pane_reply(dst_pane, dst_mtx, dst_cv);
    CK_CHECK(rep_s.state == FilesPaneReplyState::Ready && rep_d.state == FilesPaneReplyState::Ready);

    const auto mark_s = src_pane.change(nav_s.ticket, rep_s.stamp,
                                        FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 10});
    CK_CHECK(mark_s.state == FilesPaneSubmission::Accepted);
    const auto marked_s = wait_pane_reply(src_pane, src_mtx, src_cv);
    CK_CHECK(marked_s.state == FilesPaneReplyState::Ready);

    // Obtain valid reviewed evidence + consent receipt once.
    FilesOperationPlanningService planning;
    FilesOperationPanePlanningRequest preq;
    preq.operation = FileOperation::Copy;
    preq.source_service = &src_pane;
    preq.destination_service = &dst_pane;
    preq.source_navigation = nav_s.ticket;
    preq.destination_navigation = nav_d.ticket;
    preq.source_stamp = marked_s.stamp;
    preq.destination_stamp = rep_d.stamp;
    preq.source_limits.access = EntryAccess::ContentRead;
    preq.evidence_limits.max_items = preq.source_limits.held_entries;
    preq.projection_policy.require_unhandled_metadata_consent = true;
    CK_CHECK(planning.request(preq).state == FilesOperationPlanningSubmission::Accepted);

    std::optional<FilesOperationPlanningReply> planned;
    const auto p_deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
    while (!planned && std::chrono::steady_clock::now() < p_deadline) {
        planned = planning.take();
        if (!planned)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    planning.request_stop();
    planning.join();
    CK_CHECK(planned && planned->evidence && planned->plan);
    if (!planned || !planned->evidence || !planned->plan)
        return;

    std::vector<std::size_t> issues;
    for (std::size_t idx = 0; idx < planned->plan->issues().size(); ++idx)
        issues.push_back(idx);
    auto receipt_obs = planned->plan->bind(ConsentAction::ContinueWithListedLosses, std::move(issues));
    CK_CHECK(receipt_obs.value() != nullptr);
    if (!receipt_obs.value())
        return;
    const auto receipt = std::move(receipt_obs).take_value();

    auto make_exec_req = [&](std::uint8_t tag) {
        FilesOperationExecutionRequest req{
            FileOperation::Copy, &src_pane, &dst_pane,
            nav_s.ticket, nav_d.ticket, marked_s.stamp, rep_d.stamp,
            preq.source_limits, host_address(journals), {}, {}, {}, {}, {}};
        req.journal_id.bytes[0] = std::byte{0xE1};
        req.journal_id.bytes[1] = std::byte{tag};
        req.open_journal = [](const NativeAbsolutePath &root, const NativeName &jname, bool create_new) {
            return open_native_operation_journal_file(root, jname, create_new);
        };
        req.reviewed = planned->evidence;
        req.receipt = receipt;
        return req;
    };

    // Phase A: 25 rapid submit + immediate cancel cycles on FilesOperationExecutionService.
    {
        std::mutex emtx;
        std::condition_variable ecv;
        FilesOperationExecutionService exec([&] { ecv.notify_all(); });
        for (int iter = 0; iter < 25; ++iter) {
            auto sub = exec.request(make_exec_req(static_cast<std::uint8_t>(iter + 1)));
            if (sub.state == FilesOperationExecutionSubmission::Accepted) {
                (void)exec.cancel(sub.ticket);
            }
            const auto deadline = std::chrono::steady_clock::now() + kSanitizerAwareTimeout;
            while (exec.working() && std::chrono::steady_clock::now() < deadline) {
                if (exec.take())
                    break;
                std::unique_lock lock(emtx);
                ecv.wait_for(lock, std::chrono::milliseconds(2));
            }
            (void)exec.take();
        }
        CK_CHECK(exec.running());
        exec.request_stop();
        exec.join();
    }

    // Phase B: Destroy FilesOperationExecutionService while work is in-flight; 0 late wakes.
    for (int round = 0; round < 6; ++round) {
        auto owner_destroyed = std::make_shared<std::atomic<bool>>(false);
        auto late_wakes = std::make_shared<std::atomic<unsigned>>(0U);
        auto exec = std::make_unique<FilesOperationExecutionService>(
            [owner_destroyed, late_wakes] {
                if (owner_destroyed->load(std::memory_order_acquire))
                    late_wakes->fetch_add(1U, std::memory_order_relaxed);
            });
        const auto sub = exec->request(make_exec_req(static_cast<std::uint8_t>(0x80 + round)));
        CK_CHECK(sub.state == FilesOperationExecutionSubmission::Accepted);
        exec.reset();
        owner_destroyed->store(true, std::memory_order_release);
        CK_CHECK(late_wakes->load(std::memory_order_acquire) == 0U);
    }

    // Source file must remain completely untouched after cancel/destruction stress.
    std::string src_content;
    CK_CHECK(read_native_file_bytes(src_dir / "item.bin", src_content));
    CK_CHECK(src_content == "EXECUTION-CANCEL-STRESS-DATA");
}

// ===========================================================================
// 3. Real process-crash (SIGKILL / TerminateProcess) at operation boundaries
// ===========================================================================

CK_TEST(wp07_real_process_crash_staging_copy_preserves_bytes_and_blocks_overlap) {
    verify_real_process_crash_recovery("staging_copy", 0x11U);
}

CK_TEST(wp07_real_process_crash_journal_move_preserves_bytes_and_blocks_overlap) {
    verify_real_process_crash_recovery("journal_move", 0x22U);
}

// ===========================================================================
// 4. Low-memory / bounded & oversized input resilience
// ===========================================================================

CK_TEST(wp07_recovery_and_settings_oversized_and_bounded_input_resilience) {
    // Part A: Oversized / excessive journals under tight FilesOperationRecoveryLimits.
    const ck::testing::ScratchDirectory scratch("wp07-bounded-recovery", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto journals_dir = scratch.path() / "journals";
    CK_CHECK(create_private_dir(journals_dir));
    const auto journals_addr = host_address(journals_dir);
    CK_CHECK(prepare_native_operation_journal_root(journals_addr).value());

    // Write 4 journal files including an oversized 128 KiB corrupted journal.
    const std::string oversized_garbage(128U * 1024U, 'Z');
    for (int idx = 0; idx < 4; ++idx) {
        const auto fname = ".ck-copy-journal-oversized-" + std::to_string(idx);
        const auto jname = native_leaf_ascii(fname);
        auto opened = open_native_operation_journal_file(journals_addr, jname, true);
        CK_CHECK(opened.value() != nullptr);
        if (!opened.value())
            return;
        auto file = std::move(opened).take_value();
        const std::string_view payload = (idx == 0) ? std::string_view(oversized_garbage) : std::string_view("corrupt");
        const auto raw_bytes = std::as_bytes(std::span(payload.data(), payload.size()));
        std::size_t off = 0;
        while (off < raw_bytes.size()) {
            const auto w = file->write_at(off, raw_bytes.subspan(off));
            CK_CHECK(w.value() && *w.value() > 0U);
            if (!w.value() || *w.value() == 0U)
                break;
            off += *w.value();
        }
        CK_CHECK(file->flush().value() && *file->flush().value());
        CK_CHECK(file->finish_io().value() && *file->finish_io().value());
    }

    FilesOperationRecoveryLimits tight_limits{};
    tight_limits.max_journals = 2U;
    tight_limits.max_scan_batches = 2U;
    tight_limits.journal.max_evidence_bytes = 1024U;
    tight_limits.journal.max_records = 16U;

    const auto bounded_scan = discover_operation_recovery(journals_addr, tight_limits);
    // Must fail closed cleanly (either unavailable LimitExceeded or scan requiring reconciliation).
    if (bounded_scan.value()) {
        CK_CHECK(bounded_scan.value()->requires_reconciliation);
        CK_CHECK(bounded_scan.value()->journals.size() <= tight_limits.max_journals);
    } else {
        CK_CHECK(bounded_scan.unavailable_detail() != nullptr);
    }

    // Large synthetic scan (256 untrusted items) passed to assess_operation_recovery_overlap fails closed cleanly.
    FilesOperationRecoveryScan large_scan;
    large_scan.requires_reconciliation = true;
    large_scan.journals.reserve(256U);
    for (int idx = 0; idx < 256; ++idx) {
        FilesOperationRecoveryItem item;
        item.display_name = ".ck-copy-journal-synthetic-" + std::to_string(idx);
        item.error = UnavailableObservation{
            ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "wp07.synthetic_limit"}};
        large_scan.journals.push_back(std::move(item));
    }

    FrozenInventoryBuilder inv_builder(InventoryMountPolicy::DirectObjects, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    CK_CHECK(inv_builder.add({take_obs(NativeAbsolutePath::from_posix("/wp07/bounded.bin"), "bounded"),
                              {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(99),
                               NativeIdentifier::from_u64(100), {}},
                              ObjectKind::RegularFile, {}, 10U, LinkTraversalEvidence::NotLink, std::nullopt, {}}));
    CK_CHECK(inv_builder.seal());
    const auto bounded_inv = take_obs(inv_builder.completed(), "bounded_inv");

    const auto large_overlap = assess_operation_recovery_overlap(large_scan, *bounded_inv);
    CK_CHECK(large_overlap.has_conflict());
    CK_CHECK(large_overlap.has_untrusted_journal());

    // Part B: FilesSettings bounded & oversized input resilience.
    // 1. Oversized raw text (> 1 MiB) is rejected cleanly by parse_files_settings.
    std::string huge_settings = "CKFILES_SETTINGS 1\n";
    huge_settings.reserve(1200U * 1024U);
    while (huge_settings.size() < 1100U * 1024U)
        huge_settings.append("BOOKMARK 0 P 2f726f6f74\n");
    huge_settings.append("END\n");
    CK_CHECK(!parse_files_settings(huge_settings).has_value());

    // 2. Oversized single line (512 KiB) is rejected cleanly.
    const std::string huge_line = "CKFILES_SETTINGS 1\nVIEW 0 0 0 0 0 0 " + std::string(512U * 1024U, 'a') + "\nEND\n";
    CK_CHECK(!parse_files_settings(huge_line).has_value());

    // 3. FilesSettingsStore rejects oversized file on load without adopting overwrite authority,
    //    and rejects oversized settings on save while preserving prior valid contents.
    ckv::MemoryFileSystem mem_fs;
    mem_fs.add_file("/wp07-oversized-settings", std::string(1024U * 1024U + 64U, 'X'));
    const auto fp_before = mem_fs.fingerprint("/wp07-oversized-settings");
    FilesSettingsStore oversized_store(mem_fs, "/wp07-oversized-settings");
    const auto load_res = oversized_store.load();
    CK_CHECK(!load_res.diagnostic.empty());
    CK_CHECK(!oversized_store.save({}).empty());
    CK_CHECK(mem_fs.fingerprint("/wp07-oversized-settings") == fp_before);

    // Valid round-trip on clean path still succeeds.
    FilesSettingsStore valid_store(mem_fs, "/wp07-valid-settings");
    CK_CHECK(valid_store.load().diagnostic.empty());
    FilesPersistentSettings valid_settings;
    valid_settings.views[0].sort = PaneSortKey::Size;
    valid_settings.views[0].descending = true;
    CK_CHECK(valid_store.save(valid_settings).empty());
    const auto reloaded = valid_store.load();
    CK_CHECK(reloaded.diagnostic.empty());
    CK_CHECK(reloaded.value.views[0].sort == PaneSortKey::Size && reloaded.value.views[0].descending);
}

CK_TEST(wp07_cross_process_sync_session_coordination_blocks_overlapping_consumer) {
    const ck::testing::ScratchDirectory scratch("wp07-b05-sync-coord", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto coord = scratch.path() / "coord";
    const auto left = scratch.path() / "left";
    const auto right = scratch.path() / "right";
    CK_CHECK(create_private_dir(coord));
    CK_CHECK(create_private_dir(left));
    CK_CHECK(create_private_dir(right));

    const auto coord_addr = host_address(coord);
    const auto left_addr = host_address(left);
    const auto right_addr = host_address(right);
    CK_CHECK(prepare_native_operation_journal_root(coord_addr).value());

    bool saw_same_contention = false;
    bool saw_reversed_contention = false;
    CK_CHECK(spawn_and_crash_child("sync_session_lock", scratch.path(), [&] {
        // While the child process holds OS-level root locks, overlapping consumer
        // acquisition (same order or reversed roots) must fail closed with Changed.
        const auto same = open_native_sync_session_backend(coord_addr, left_addr, right_addr);
        saw_same_contention = (!same.value() && same.state() == ObservationState::Changed);
        const auto reversed = open_native_sync_session_backend(coord_addr, right_addr, left_addr);
        saw_reversed_contention = (!reversed.value() && reversed.state() == ObservationState::Changed);
    }));
    CK_CHECK(saw_same_contention);
    CK_CHECK(saw_reversed_contention);

    // After the child is abruptly terminated (SIGKILL / TerminateProcess), the OS
    // releases its crash-abandoned locks and the next consumer acquires cleanly.
    auto recovered = open_native_sync_session_backend(coord_addr, left_addr, right_addr);
    CK_CHECK(recovered.value() != nullptr);
    if (recovered.value()) {
        const auto finished = (*recovered.value())->finish_io();
        CK_CHECK(finished.value() && *finished.value());
    }
}

CK_TEST(wp07_cross_process_ui_copy_blocked_by_peer_root_lease_and_recovers) {
    const ck::testing::ScratchDirectory scratch("wp07-b05-ui-copy-lease", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto coord = scratch.path() / "coord";
    const auto left = scratch.path() / "left";
    const auto right = scratch.path() / "right";
    const auto cache_l = scratch.path() / "cache-l";
    const auto cache_r = scratch.path() / "cache-r";
    CK_CHECK(create_private_dir(coord));
    CK_CHECK(create_private_dir(left));
    CK_CHECK(create_private_dir(right));
    CK_CHECK(create_private_dir(cache_l));
    CK_CHECK(create_private_dir(cache_r));
    {
        std::ofstream out(left / "doc.txt", std::ios::binary);
        out << "b05-cross-process-payload";
    }

    const auto coord_addr = host_address(coord);
    const auto left_addr = host_address(left);
    const auto right_addr = host_address(right);
    CK_CHECK(prepare_native_operation_journal_root(coord_addr).value());

    std::mutex mtx_l, mtx_r;
    std::condition_variable cv_l, cv_r;
    FilesPaneService src_pane(native_files_pane_backend(host_address(cache_l)), {}, [&] { cv_l.notify_all(); });
    FilesPaneService dst_pane(native_files_pane_backend(host_address(cache_r)), {}, [&] { cv_r.notify_all(); });
    FilesOperationPlanningService planner;
    FilesOperationExecutionService executor;

    const auto nav_l = src_pane.navigate(left_addr);
    const auto nav_r = dst_pane.navigate(right_addr);
    CK_CHECK(nav_l.state == FilesPaneSubmission::Accepted && nav_r.state == FilesPaneSubmission::Accepted);
    const auto rep_l = wait_pane_reply(src_pane, mtx_l, cv_l);
    const auto rep_r = wait_pane_reply(dst_pane, mtx_r, cv_r);
    CK_CHECK(rep_l.state == FilesPaneReplyState::Ready && rep_r.state == FilesPaneReplyState::Ready);

    const auto plan_once = [&] {
        FilesOperationPanePlanningRequest req;
        req.operation = FileOperation::Move;
        req.source_service = &src_pane;
        req.destination_service = &dst_pane;
        req.source_navigation = rep_l.navigation;
        req.destination_navigation = rep_r.navigation;
        req.source_stamp = rep_l.stamp;
        req.destination_stamp = rep_r.stamp;
        req.source_limits.held_entries = 64;
        req.source_limits.access = EntryAccess::MetadataOnly;
        CK_CHECK(planner.request(std::move(req)).state == FilesOperationPlanningSubmission::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto r = planner.take())
                return *r;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("planner timed out");
    };

    const auto run_copy = [&](const FilesOperationPlanningReply &planned) {
        std::vector<std::size_t> issues;
        for (std::size_t idx = 0; idx < planned.plan->issues().size(); ++idx)
            issues.push_back(idx);
        const auto action = issues.empty() ? ConsentAction::Proceed : ConsentAction::ContinueWithListedLosses;
        auto receipt_obs = planned.plan->bind(action, std::move(issues));
        CK_CHECK(receipt_obs.value() != nullptr);
        ConsentReceipt receipt = std::move(receipt_obs).take_value();
        FilesOperationExecutionRequest exec{
            FileOperation::Move, &src_pane, &dst_pane,
            rep_l.navigation, rep_r.navigation, rep_l.stamp, rep_r.stamp,
            {}, coord_addr, {}, {},
            [](const NativeAbsolutePath &r, const NativeName &n, bool create) {
                return open_native_operation_journal_file(r, n, create);
            },
            planned.evidence, receipt, false};
        exec.source_limits.held_entries = 64;
        exec.source_limits.access = EntryAccess::MetadataOnly;
        CK_CHECK(executor.request(std::move(exec)).state == FilesOperationExecutionSubmission::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto r = executor.take())
                return *r;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("executor timed out");
    };

    const auto planned = plan_once();
    CK_CHECK(planned.evidence && planned.plan);

    // While a peer process holds OS root locks on left/right, non-sync UI Copy MUST fail closed.
    bool blocked_by_peer = false;
    CK_CHECK(spawn_and_crash_child("sync_session_lock", scratch.path(), [&] {
        const auto blocked = run_copy(planned);
        blocked_by_peer = (blocked.state == FilesOperationExecutionState::Failed &&
                           blocked.error && blocked.error->state == ObservationState::Changed);
    }));
    CK_CHECK(blocked_by_peer);
    CK_CHECK(!std::filesystem::exists(right / "doc.txt"));

    // After the peer terminates and OS releases locks, the exact same UI Copy succeeds cleanly.
    const auto succeeded = run_copy(planned);
    CK_CHECK(succeeded.state == FilesOperationExecutionState::Complete);
    CK_CHECK(std::filesystem::exists(right / "doc.txt"));
}

CK_TEST(wp07_b04_cross_volume_move_retires_stable_source_and_retains_modified_or_hardlinked_source) {
    const ck::testing::ScratchDirectory scratch("wp07-b04-move-retire", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto src = scratch.path() / "src";
    const auto dst = scratch.path() / "dst";
    const auto journals = scratch.path() / "journals";
    CK_CHECK(create_private_dir(src));
    CK_CHECK(create_private_dir(dst));
    CK_CHECK(create_private_dir(journals));
    const auto journals_addr = host_address(journals);
    CK_CHECK(prepare_native_operation_journal_root(journals_addr).value());

    const auto make_name = [](const char *p, const char16_t *w) {
#if defined(_WIN32)
        (void)p;
        return *NativeName::from_windows(w);
#else
        (void)w;
        return *NativeName::from_posix(p);
#endif
    };

    NativeFileSystem fs;
    auto src_dir = std::shared_ptr<DirectoryCursor>(take_obs(fs.open_directory(src), "open_src"));
    auto dst_dir = std::shared_ptr<DirectoryCursor>(take_obs(fs.open_directory(dst), "open_dst"));
    const auto src_dir_id = take_obs(src_dir->identity(), "src_dir_id");
    const auto dst_dir_id = take_obs(dst_dir->identity(), "dst_dir_id");

    // Case 1: Unchanged single-link regular file -> cross-volume style Move proves content+metadata
    // stability and retires the source cleanly.
    CK_CHECK(std::filesystem::exists(src));
    {
        std::ofstream out(src / "stable.bin", std::ios::binary);
        out << "stable-move-payload-12345";
    }
    auto stable_move_open = src_dir->open_child(make_name("stable.bin", u"stable.bin"),
                                                EntryAccess::ContentRead | EntryAccess::MoveSource);
    CK_CHECK(stable_move_open.value() != nullptr);
    if (stable_move_open.value()) {
        const auto stable_ok = (*stable_move_open.value())->qualify_stable_content();
        CK_CHECK(stable_ok.value() && *stable_ok.value());
    }
    stable_move_open = Observation<std::unique_ptr<OpenedEntry>>::unavailable(
        ObservationState::Gone, {ErrorDomain::Adapter, 0, "release"});

#if !defined(__APPLE__)
    auto stable_entry = take_obs(src_dir->open_child(make_name("stable.bin", u"stable.bin"), EntryAccess::MetadataOnly),
                                 "stable_entry");
    const auto stable_id = take_obs(take_obs(stable_entry->inspect_basic(), "stable_meta").identity, "stable_id");
    stable_entry.reset();

    auto jid1 = take_obs(new_native_operation_journal_id(), "jid1");
    auto j1 = take_obs(OperationJournal::create(FileOperation::Move, jid1, {4, 32, 64 * 1024}), "j1");
    auto jf1 = std::shared_ptr<OperationJournalFile>(take_obs(
        open_native_operation_journal_file(journals_addr, make_name(".ck-move-b04-1", u".ck-move-b04-1"), true), "jf1"));
    CK_CHECK(append_operation_journal_file(*jf1, *j1, {1, 0, OperationJournalState::Planned, {}}).value());
    NativeOperationCopyRequest req1{
        j1, jf1, 0, src_dir, src_dir_id, make_name("stable.bin", u"stable.bin"), stable_id,
        dst_dir, dst_dir_id, host_address(dst), make_name(".ck-stage-b04-1", u".ck-stage-b04-1"),
        make_name("stable.bin", u"stable.bin"), {},
        [](OpenedEntry &staging, std::stop_token) {
            OperationMetadataRequest m;
            m.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
            m.apply_security_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
            m.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
            m.flush_destination = [&staging](std::stop_token) { return staging.flush_content(); };
            return OperationMetadataJob::create(std::move(m));
        },
        FileOperation::Move, {}, {}, {}, {}, {}};
    auto job1 = take_obs(NativeOperationCopyJob::create(std::move(req1)), "job1");
    while (job1->state() != OperationExecutionState::Complete &&
           job1->state() != OperationExecutionState::Failed &&
           job1->state() != OperationExecutionState::RetainedSource &&
           job1->state() != OperationExecutionState::NeedsReconciliation)
        job1->advance();
    if (job1->state() != OperationExecutionState::Complete && job1->result().failure) {
        std::fprintf(stderr, "B-04 job1 failed: state=%d op=%s code=%lld\n",
                     static_cast<int>(job1->state()),
                     job1->result().failure->error.operation.c_str(),
                     static_cast<long long>(job1->result().failure->error.code));
    }
    CK_CHECK(job1->state() == OperationExecutionState::Complete);
    CK_CHECK(!std::filesystem::exists(src / "stable.bin"));
    CK_CHECK(std::filesystem::exists(dst / "stable.bin"));
    (void)jf1->finish_io();
#else
    (void)dst_dir;
    (void)src_dir_id;
    (void)dst_dir_id;
    // On macOS APFS (B-01), verify MoveSource detects concurrent content/metadata change and refuses retirement.
    auto watched = take_obs(src_dir->open_child(make_name("stable.bin", u"stable.bin"),
                                                EntryAccess::ContentRead | EntryAccess::MoveSource), "watched");
    CK_CHECK(watched->qualify_stable_content().value());
    {
        std::ofstream mutate(src / "stable.bin", std::ios::binary | std::ios::app);
        mutate << "-mutated";
    }
    const auto after_mutate = watched->qualify_stable_content();
    CK_CHECK(!after_mutate.value() && after_mutate.state() == ObservationState::Changed);
    watched.reset();
#endif

    // Case 2: Hard-linked source (link_count > 1) -> refused without deleting source.
    {
        std::ofstream out(src / "linked.bin", std::ios::binary);
        out << "hardlinked-payload";
    }
    std::filesystem::create_hard_link(src / "linked.bin", src / "linked-alias.bin");
    auto linked_open = src_dir->open_child(make_name("linked.bin", u"linked.bin"),
                                           EntryAccess::ContentRead | EntryAccess::MoveSource);
    if (linked_open.value()) {
        const auto q = (*linked_open.value())->qualify_stable_content();
        CK_CHECK(!q.value());
    } else {
        CK_CHECK(linked_open.state() == ObservationState::Unsupported);
    }
    CK_CHECK(std::filesystem::exists(src / "linked.bin"));
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] != nullptr)
        g_argv0 = argv[0];

    if (const auto child_mode = get_env_string("CKFILES_WP07_CRASH_CHILD")) {
        const auto child_root = get_env_string("CKFILES_WP07_CRASH_ROOT");
        if (!child_root || child_root->empty())
            return 20;
        return run_crash_child(*child_mode, std::filesystem::path(*child_root));
    }

    return ::cktest::run_all(argc, argv);
}
