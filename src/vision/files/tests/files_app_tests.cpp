// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include "files_location_dialog.hpp"
#include "files_settings_service.hpp"
#include "files_location_text.hpp"
#include "native_filesystem.hpp"
#include "sha256.hpp"
#include "scratch_directory.hpp"
#include "files_native_fixture_snapshot.hpp"
#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/testing/cktest.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/label.hpp>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
namespace {
using namespace ck::vision;
using namespace ck::files;

template <class T>
void append_little_endian(std::vector<std::byte> &out, T value) {
    for (std::size_t index = 0; index != sizeof(T); ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

void append_length_string(std::vector<std::byte> &out, std::string_view value) {
    append_little_endian<std::uint64_t>(out, value.size());
    for (const auto byte : value)
        out.push_back(std::byte{static_cast<unsigned char>(byte)});
}

void append_native_path(std::vector<std::byte> &out, const NativeAbsolutePath &path) {
    append_little_endian<std::uint8_t>(out, static_cast<std::uint8_t>(path.encoding()));
    if (path.posix_bytes()) {
        append_length_string(out, *path.posix_bytes());
        return;
    }
    std::string bytes;
    for (const auto unit : *path.windows_units()) {
        bytes.push_back(static_cast<char>(unit & 0xff));
        bytes.push_back(static_cast<char>(unit >> 8));
    }
    append_length_string(out, bytes);
}

std::vector<std::byte> archive_recovery_evidence(
    std::uint64_t transaction, const NativeAbsolutePath &destination,
    const NativeAbsolutePath &staging, std::span<const std::byte> digest) {
    std::vector<std::byte> result;
    append_little_endian<std::uint64_t>(result, transaction);
    append_length_string(result, "fixture-volume");
    append_length_string(result, "/fixture");
    append_length_string(result, "fixture.ckarchive");
    append_native_path(result, destination);
    append_little_endian<std::uint8_t>(result, 3U);
    append_native_path(result, staging);
    result.insert(result.end(), digest.begin(), digest.end());
    return result;
}

std::array<std::byte, 32> digest_bytes(std::span<const std::byte> bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.digest();
}

template <class T> void find_views(ckv::ui::View &root, std::vector<T *> &found) {
    if (auto *view = dynamic_cast<T *>(&root))
        found.push_back(view);
    for (const auto &child : root.children())
        find_views(*child, found);
}
NativeAbsolutePath address(const std::filesystem::path &path) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value())
        throw std::runtime_error("invalid app fixture address");
    return std::move(value).take_value();
}
OperationReviewModel::Pointer review_model() {
    FrozenOperationItem source{ObjectIdentity{AdapterScope{}, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}}, ObjectKind::RegularFile,
        SourceTargetTopology::Disjoint, std::nullopt};
    auto plan = OperationPlan::create(FileOperation::Copy, {std::move(source)}, {},
        {{0, DestinationConflictKind::ExistingFile}});
    if (!plan.value())
        throw std::runtime_error("Files app review fixture plan unavailable");
    auto model = OperationReviewModel::create(std::move(plan).take_value());
    if (!model.value())
        throw std::runtime_error("Files app review fixture model unavailable");
    return std::move(model).take_value();
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
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
        if (!changed.wait_for(lock, timeout, [&] { return entered; }))
            throw std::runtime_error("blocked app worker did not enter gate");
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
class ReadGateFile final : public IndexByteFile {
    std::unique_ptr<IndexByteFile> file_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<std::atomic<bool>> armed_;

  public:
    ReadGateFile(std::unique_ptr<IndexByteFile> file, std::shared_ptr<Gate> gate,
                 std::shared_ptr<std::atomic<bool>> armed)
        : file_(std::move(file)), gate_(std::move(gate)), armed_(std::move(armed)) {}
    Observation<std::uint64_t> size() override { return file_->size(); }
    Observation<std::size_t> write_at(std::uint64_t at, std::span<const std::byte> bytes) override {
        return file_->write_at(at, bytes);
    }
    Observation<std::size_t> read_at(std::uint64_t at, std::span<std::byte> bytes) override {
        if (armed_->exchange(false))
            gate_->block();
        return file_->read_at(at, bytes);
    }
};
class ReadGateFactory final : public IndexFileFactory {
    std::shared_ptr<IndexFileFactory> factory_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<std::atomic<bool>> armed_;

  public:
    ReadGateFactory(std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<Gate> gate,
                    std::shared_ptr<std::atomic<bool>> armed)
        : factory_(std::move(factory)), gate_(std::move(gate)), armed_(std::move(armed)) {}
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        auto result = factory_->create_file();
        if (!result.value())
            return result;
        return Observation<std::unique_ptr<IndexByteFile>>::available(
            std::make_unique<ReadGateFile>(std::move(result).take_value(), gate_, armed_));
    }
};
struct Fixture {
    ck::testing::ScratchDirectory scratch{"files-app", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT};
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal{ckv::Size{120, 34}};
    ckv::ui::Application application{terminal, clock};
    FilesAppWake wake{application};
    std::shared_ptr<Gate> gate;
    std::shared_ptr<Gate> directory_gate = std::make_shared<Gate>();
    std::shared_ptr<std::atomic<bool>> block_directory = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<Gate> home_gate = std::make_shared<Gate>();
    std::shared_ptr<std::atomic<bool>> block_home = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> fail_home = std::make_shared<std::atomic<bool>>(false);
    std::array<std::unique_ptr<FilesPaneService>, 2> services;
    std::array<std::unique_ptr<FilesCompletionService>, 2> completion_services;
    std::unique_ptr<FilesOperationPlanningService> planning_service;
    std::unique_ptr<FilesOperationExecutionService> execution_service;
    std::unique_ptr<FilesVolumeService> volume_service;
    std::unique_ptr<FilesVolumeArchiveService> volume_archive_service;
    std::unique_ptr<FilesVolumeArchiveRecoveryService> volume_archive_recovery_service;
    std::unique_ptr<FilesVolumeArchiveRecoveryCleanupService> volume_archive_recovery_cleanup_service;
    std::unique_ptr<FilesOperationRecoveryService> operation_recovery_service;
    std::unique_ptr<FilesOperationRecoveryCleanupService> operation_recovery_cleanup_service;
    std::unique_ptr<FilesApp> app;
    unsigned exits = 0;
    explicit Fixture(ThemeScheme theme = ThemeScheme::Classic, std::shared_ptr<Gate> blocked = {},
                     FilesPaneServiceLimits limits = {}, ckv::Size size = {120, 34},
                     std::shared_ptr<std::atomic<bool>> fail_notification = {}, bool folders = false,
                     bool missing_initial = false, bool rejected_initial = false, FilesSettingsBinding settings = {},
                     bool qualified_volume = false, bool recovery_journal = false,
                     bool operation_recovery_journal = false, bool eligible_archive_recovery = false,
                     bool eligible_operation_recovery = false,
                     std::function<Observation<std::shared_ptr<IndexFileFactory>>()> completion_workspace = {})
        : terminal(size), gate(std::move(blocked)) {
#ifndef _WIN32
        std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
        for (std::size_t i = 0; i < 2; ++i) {
            const auto dir = scratch.path() / (i == 0 ? "left" : "right");
            const auto cache = scratch.path() / (i == 0 ? "cache-left" : "cache-right");
            std::filesystem::create_directory(dir);
            std::filesystem::create_directory(cache);
#ifndef _WIN32
            std::filesystem::permissions(cache, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace);
#endif
            for (int n = 0; n < (i == 0 ? 100 : 3); ++n) {
                std::ofstream file(dir / ((i == 0 ? "left-item-" : "right-item-") + std::to_string(n)));
                file << n;
                if (!file)
                    throw std::runtime_error("cannot write app fixture");
            }
            if (folders) {
                const auto folder = dir / (i == 0 ? "left-item-0" : "right-item-0");
                std::filesystem::remove(folder);
                std::filesystem::create_directory(folder);
                std::filesystem::create_directory(folder / "nested");
                {
                    std::ofstream file(folder / "inside.txt");
                    file << "inside";
                }
                {
                    std::ofstream file(folder / "nested" / "leaf.txt");
                    file << "leaf";
                }
            }
            auto backend = native_files_pane_backend(address(cache));
            backend.home = [home = address(scratch.path() / "right"), hold = home_gate, arm = block_home,
                            fail = fail_home](NativePathLimits, std::stop_token) {
                if (arm->exchange(false))
                    hold->block();
                if (fail->load())
                    return Observation<NativeAbsolutePath>::unavailable(ObservationState::Denied,
                                                                        {ErrorDomain::Adapter, 42, "injected.home"});
                return Observation<NativeAbsolutePath>::available(home);
            };
            if (i == 0) {
                backend.workspace = [open = std::move(backend.workspace), hold = directory_gate,
                                     arm = block_directory]() -> Observation<std::shared_ptr<IndexFileFactory>> {
                    auto result = open();
                    if (!result.value())
                        return result;
                    return Observation<std::shared_ptr<IndexFileFactory>>::available(
                        std::make_shared<ReadGateFactory>(*result.value(), hold, arm));
                };
            }
            if (i == 0 && gate) {
                backend.filesystem = [create = std::move(backend.filesystem), hold = gate] {
                    hold->block();
                    return create();
                };
            }
            auto notify = wake.callback();
            if (i == 0 && fail_notification)
                notify = [signal = std::move(notify), fault = fail_notification] {
                    if (fault->exchange(false))
                        throw std::runtime_error("injected acknowledgment notification failure");
                    signal();
                };
            services[i] = std::make_unique<FilesPaneService>(std::move(backend), limits, std::move(notify));
            const auto completion_cache = scratch.path() / (i == 0 ? "completion-cache-left" : "completion-cache-right");
            std::filesystem::create_directory(completion_cache);
#ifndef _WIN32
            std::filesystem::permissions(completion_cache, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace);
#endif
            auto completion_backend = native_files_pane_backend(address(completion_cache));
            if (i == 0 && completion_workspace)
                completion_backend.workspace = completion_workspace;
            completion_services[i] =
                std::make_unique<FilesCompletionService>(std::move(completion_backend),
                                                         FilesCompletionServiceLimits{}, wake.callback());
        }
        const auto operation_root = scratch.path() / "operation-journals";
        std::filesystem::create_directory(operation_root);
#ifndef _WIN32
        std::filesystem::permissions(operation_root, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
        const auto operation_root_address = address(operation_root);
        auto prepared_journal_root = prepare_native_operation_journal_root(operation_root_address);
        if (!prepared_journal_root.value())
            throw std::runtime_error("cannot prepare app operation journal root");
        if (eligible_archive_recovery) {
            const auto destination = scratch.path() / "archive-destination";
            std::filesystem::create_directory(destination);
            const auto staging = destination / ".fixture.ckarchive.cktmp-31";
            const std::array<std::byte, 13> content{
                std::byte{'a'}, std::byte{'r'}, std::byte{'c'}, std::byte{'h'}, std::byte{'i'},
                std::byte{'v'}, std::byte{'e'}, std::byte{'-'}, std::byte{'s'}, std::byte{'t'},
                std::byte{'a'}, std::byte{'g'}, std::byte{'e'}};
            {
                std::ofstream file(staging, std::ios::binary);
                file.write(reinterpret_cast<const char *>(content.data()),
                           static_cast<std::streamsize>(content.size()));
            }
            const auto evidence = archive_recovery_evidence(
                31, address(destination), address(staging), digest_bytes(content));
            OperationJournalId id;
            id.bytes[0] = std::byte{31};
            auto created = OperationJournal::create(FileOperation::Copy, id);
            if (!created.value())
                throw std::runtime_error("cannot create eligible archive recovery journal");
#if defined(_WIN32)
            const auto journal_name = NativeName::from_windows(
                u".ck-volume-archive-1f000000000000000000000000000000");
#else
            const auto journal_name = NativeName::from_posix(
                ".ck-volume-archive-1f000000000000000000000000000000");
#endif
            if (!journal_name)
                throw std::runtime_error("cannot encode eligible archive recovery journal name");
            auto file = open_native_operation_journal_file(operation_root_address, *journal_name, true);
            if (!file.value())
                throw std::runtime_error("cannot open eligible archive recovery journal");
            auto journal = std::move(created).take_value();
            for (const auto &[sequence, state] : std::array{
                     std::pair<std::uint64_t, OperationJournalState>{1, OperationJournalState::Planned},
                     std::pair<std::uint64_t, OperationJournalState>{2, OperationJournalState::Prepared},
                     std::pair<std::uint64_t, OperationJournalState>{3, OperationJournalState::Copied}}) {
                if (!append_operation_journal_file(**file.value(), *journal,
                                                   {sequence, 0, state, evidence}).value())
                    throw std::runtime_error("cannot append eligible archive recovery journal");
            }
        } else if (recovery_journal) {
            std::ofstream journal(operation_root / ".ck-volume-archive-00000000000000000000000000000031",
                                  std::ios::binary);
            journal << "malformed recovery journal";
            if (!journal)
                throw std::runtime_error("cannot create app recovery fixture");
        }
        if (operation_recovery_journal) {
            std::ofstream journal(operation_root / ".ck-copy-journal-00000000000000000000000000000032",
                                  std::ios::binary);
            journal << "malformed operation recovery journal";
            if (!journal)
                throw std::runtime_error("cannot create operation recovery fixture");
        }
        if (eligible_operation_recovery) {
            const auto destination = scratch.path() / "operation-destination";
            std::filesystem::create_directory(destination);
            const auto destination_address = address(destination);
            NativeFileSystem filesystem;
            auto destination_result = filesystem.open_native_directory(destination_address);
            if (!destination_result.value())
                throw std::runtime_error("cannot open operation recovery destination");
            auto held_destination = std::shared_ptr<const DirectoryCursor>(std::move(destination_result).take_value());
#if defined(_WIN32)
            const auto staging_name = NativeName::from_windows(u".ck-copy-staging-recovery");
            const auto published_name = NativeName::from_windows(u"published");
            const auto journal_name = NativeName::from_windows(
                u".ck-copy-journal-1f000000000000000000000000000000");
#else
            const auto staging_name = NativeName::from_posix(".ck-copy-staging-recovery");
            const auto published_name = NativeName::from_posix("published");
            const auto journal_name = NativeName::from_posix(
                ".ck-copy-journal-1f000000000000000000000000000000");
#endif
            if (!staging_name || !published_name || !journal_name)
                throw std::runtime_error("cannot encode operation recovery fixture names");
            auto staging_result = held_destination->create_child_file(*staging_name);
            if (!staging_result.value())
                throw std::runtime_error("cannot create operation recovery staging");
            auto staging = std::move(staging_result).take_value();
            const auto staging_metadata = staging->inspect_basic();
            const auto destination_identity = held_destination->identity();
            if (!staging_metadata.value() || !staging_metadata.value()->identity.value() ||
                !destination_identity.value())
                throw std::runtime_error("cannot inspect operation recovery fixture identities");
            const auto staging_durable = NativeDurableIdentity::from_live(*staging_metadata.value()->identity.value());
            const auto destination_durable = NativeDurableIdentity::from_live(*destination_identity.value());
            if (!staging_durable || !destination_durable)
                throw std::runtime_error("cannot derive operation recovery fixture identities");
            OperationJournalId id;
            id.bytes[0] = std::byte{0x32};
            auto created = OperationJournal::create(FileOperation::Copy, id);
            if (!created.value())
                throw std::runtime_error("cannot create eligible operation recovery journal");
            NativeOperationRecoveryEvidence receipt{
                *staging_name, *published_name, *destination_durable, *destination_durable,
                *destination_durable, *staging_durable, destination_address, {}, {}};
            auto encoded = encode_native_operation_recovery_evidence(receipt);
            if (!encoded.value())
                throw std::runtime_error("cannot encode eligible operation recovery journal");
            auto file = open_native_operation_journal_file(operation_root_address, *journal_name, true);
            if (!file.value())
                throw std::runtime_error("cannot open eligible operation recovery journal");
            auto journal = std::move(created).take_value();
            for (const auto &[sequence, state] : std::array{
                     std::pair<std::uint64_t, OperationJournalState>{1, OperationJournalState::Planned},
                     std::pair<std::uint64_t, OperationJournalState>{2, OperationJournalState::Prepared},
                     std::pair<std::uint64_t, OperationJournalState>{3, OperationJournalState::Copied}}) {
                if (!append_operation_journal_file(**file.value(), *journal,
                                                   {sequence, 0, state,
                                                    state == OperationJournalState::Planned
                                                        ? std::vector<std::byte>{} : *encoded.value()}).value())
                    throw std::runtime_error("cannot append eligible operation recovery journal");
            }
        }
        execution_service = std::make_unique<FilesOperationExecutionService>(wake.callback());
        planning_service = std::make_unique<FilesOperationPlanningService>(wake.callback());
        if (qualified_volume || recovery_journal || eligible_archive_recovery) {
            volume_archive_service = std::make_unique<FilesVolumeArchiveService>(wake.callback());
            volume_archive_recovery_service = std::make_unique<FilesVolumeArchiveRecoveryService>(wake.callback());
            volume_archive_recovery_cleanup_service =
                std::make_unique<FilesVolumeArchiveRecoveryCleanupService>(wake.callback());
        }
        if (operation_recovery_journal || eligible_operation_recovery) {
            operation_recovery_service = std::make_unique<FilesOperationRecoveryService>(wake.callback());
            operation_recovery_cleanup_service =
                std::make_unique<FilesOperationRecoveryCleanupService>(wake.callback());
        }
        volume_service = std::make_unique<FilesVolumeService>(wake.callback(), [qualified_volume] {
            std::vector<FilesVolumeRecord> records;
            records.push_back({"Fixture volume", "/fixture", "fixturefs", 10ull * 1024 * 1024,
                               3ull * 1024 * 1024, false, "fixture-volume", {
                                   qualified_volume ? FilesVolumeArchiveSupport::LiveReadable
                                                    : FilesVolumeArchiveSupport::Unknown,
                                   false,
                                   qualified_volume ? "Live source traversal qualified"
                                                    : "fixture capability probe pending"}, {}});
            records.push_back({"Fixture read-only", "/fixture-ro", "readonlyfs", 4ull * 1024 * 1024,
                               2ull * 1024 * 1024, true, "fixture-read-only", {
                                   FilesVolumeArchiveSupport::Unknown, false, "fixture capability probe pending"}, {}});
            // Keep the volume viewport larger than the root's cached page.
            for (int i = 0; i < 96; ++i) {
                const auto name = "fixture-mount-" + std::to_string(i);
                records.push_back({name, "/" + name, "fixturefs", {}, {}, false, name, {}, {}});
            }
            return Observation<std::vector<FilesVolumeRecord>>::available(std::move(records));
        });
        app = std::make_unique<FilesApp>(
            application, std::array<FilesPaneService *, 2>{services[0].get(), services[1].get()}, wake,
            std::array<NativeAbsolutePath, 2>{
                address(scratch.path() / (rejected_initial ? std::string(1536, 'x')
                                                           : std::string(missing_initial ? "missing" : "left"))),
                address(scratch.path() / "right")},
            [this] { ++exits; }, theme,
            std::array<FilesCompletionService *, 2>{completion_services[0].get(), completion_services[1].get()},
            planning_service.get(), execution_service.get(),
            std::make_shared<const NativeAbsolutePath>(operation_root_address), std::move(settings),
            volume_service.get(), volume_archive_service.get(),
            volume_archive_recovery_service.get(), volume_archive_recovery_cleanup_service.get(),
            operation_recovery_service.get(), operation_recovery_cleanup_service.get());
    }
    ~Fixture() {
        app.reset();
        volume_service->join();
        if (gate)
            gate->release();
        directory_gate->release();
        home_gate->release();
        for (auto &service : services)
            service->join();
        for (auto &service : completion_services)
            service->join();
        planning_service->join();
        execution_service->join();
        if (volume_archive_service)
            volume_archive_service->join();
        if (volume_archive_recovery_service)
            volume_archive_recovery_service->join();
        if (volume_archive_recovery_cleanup_service)
            volume_archive_recovery_cleanup_service->join();
        if (operation_recovery_service)
            operation_recovery_service->join();
        if (operation_recovery_cleanup_service)
            operation_recovery_cleanup_service->join();
        application.step(0); // Drain late weak posts after controller destruction.
    }
    template <class Predicate> void until(Predicate ready) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            application.step(clock.now_nanos());
            if (ready())
                return;
            clock.advance(1'000'000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < end);
        throw std::runtime_error("Files application condition timed out: " + app->status(0) + " / " + app->status(1));
    }
    void loaded() {
        until([&] { return !app->pending(0) && !app->pending(1); });
    }
    ckv::ui::CommandId id(std::string_view name) {
        const auto value = application.commands().id_for(name);
        if (!value)
            throw std::runtime_error("missing Files test command");
        return *value;
    }
    bool command(std::string_view name) { return application.execute_command(id(name)); }
    bool key(ckv::Key value, ckv::Modifier modifiers = ckv::Modifier::None) {
        return application.dispatch(ckv::KeyEvent{{value, modifiers, {}}});
    }
    bool location_key() { return application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Ctrl, "l"}}); }
    bool alt(std::string text) {
        return application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, std::move(text)}});
    }
    ckv::widgets::InputLine *editor_input() {
        auto *input = dynamic_cast<ckv::widgets::InputLine *>(application.focused());
        if (!input)
            throw std::runtime_error("location editor did not focus its real input");
        return input;
    }
    std::string path_text(const std::filesystem::path &path) {
        const auto value = address(path);
        if (value.posix_bytes())
            return *value.posix_bytes();
        std::string text;
        for (auto unit : *value.windows_units()) {
            if (unit > 0x7f)
                throw std::runtime_error("fixture root is not ASCII");
            text.push_back(static_cast<char>(unit));
        }
        return text;
    }
    std::string painted() {
        application.step(clock.now_nanos());
        std::string text;
        const auto &frame = application.current_frame();
        for (int y = 0; y < frame.size().height; ++y) {
            for (int x = 0; x < frame.size().width; ++x)
                text += frame.at({x, y}).grapheme();
            text += '\n';
        }
        return text;
    }
};
#if defined(__APPLE__)
void expect_mac_copy_publication_refusal(Fixture& fixture, const ck::testing::FilesFixtureTree& source_before,
                                         const ck::testing::FilesFixtureTree& target_before, bool marked_batch = false) {
    fixture.until([&] {
        return !fixture.app->operation_review_pending() && !fixture.app->pending(0) && !fixture.app->pending(1) &&
               fixture.app->status(0).find("Copy needs reconciliation") != std::string::npos;
    });
    fixture.execution_service->request_stop();
    fixture.execution_service->join(); // Drain native writers/journals before independent inspection.
    CK_CHECK(ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "left") == source_before);
    CK_CHECK(fixture.app->status(0).find("verified file publication is unavailable on this platform") != std::string::npos);
    CK_CHECK(fixture.app->status(0).find("create_staging_file.fd_publication_unqualified") == std::string::npos);
    CK_CHECK(fixture.app->status(0).find("Copy completed") == std::string::npos);
    auto target_after = ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "right");
    unsigned journals = 0;
    unsigned retained_directories = 0;
    unsigned total_prepared = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.scratch.path() / "operation-journals")) {
        const auto filename = entry.path().filename().native();
        if (!filename.starts_with(".ck-copy-journal-") && !filename.starts_with(".ck-tree-journal-")) continue;
        ++journals;
        const auto journal = ck::testing::read_files_fixture_journal(entry.path());
        CK_CHECK(!journal->records().empty());
        const bool tree_journal = filename.starts_with(".ck-tree-journal-");
        std::vector<OperationJournalState> states;
        for (const auto& record : journal->records()) {
            states.push_back(record.state);
            if (record.state == OperationJournalState::Prepared) {
                ++total_prepared;
                if (tree_journal) {
                    const auto decoded = decode_native_operation_recovery_evidence(record.evidence);
                    CK_CHECK(decoded.value() && decoded.value()->staging_identity);
                    if (!decoded.value() || !decoded.value()->staging_identity)
                        throw std::runtime_error("missing retained tree staging identity");
                    const auto& receipt = *decoded.value();
                    CK_CHECK(receipt.staging_name.posix_bytes() && receipt.destination_name.posix_bytes());
                    const auto staging = *receipt.staging_name.posix_bytes();
                    CK_CHECK(staging.starts_with(".ck-tree-staging-"));
                    CK_CHECK(!std::filesystem::exists(fixture.scratch.path() / "right" / *receipt.destination_name.posix_bytes()));
                    NativeFileSystem native;
                    auto parent = native.open_directory(fixture.scratch.path() / "right");
                    if (!parent.value()) throw std::runtime_error("cannot inspect retained tree parent");
                    auto held = (*parent.value())->open_child(receipt.staging_name, EntryAccess::MetadataOnly);
                    if (!held.value()) throw std::runtime_error("cannot inspect retained tree directory");
                    auto metadata = (*held.value())->inspect_basic();
                    CK_CHECK(metadata.value() && metadata.value()->identity.value() &&
                             receipt.staging_identity->matches_live(*metadata.value()->identity.value()));
                    ++retained_directories;
                    for (auto retained = target_after.begin(); retained != target_after.end();) {
                        if (retained->first == staging || retained->first.starts_with(staging + "/")) {
                            CK_CHECK(S_ISDIR(retained->second.mode) && retained->second.bytes.empty());
                            retained = target_after.erase(retained);
                        } else ++retained;
                    }
                }
            }
        }
        if (tree_journal)
            CK_CHECK((states == std::vector<OperationJournalState>{OperationJournalState::Planned,
                OperationJournalState::Prepared, OperationJournalState::NeedsReconciliation}));
        else {
            CK_CHECK(states.size() == 1 || states.size() == 2);
            CK_CHECK(states.front() == OperationJournalState::Planned);
            if (states.size() == 2) CK_CHECK(states.back() == OperationJournalState::Prepared);
        }
    }
    CK_CHECK(journals == (marked_batch ? 2U : 1U) + retained_directories && retained_directories <= 1);
    CK_CHECK(total_prepared == 1 + retained_directories);
    if (retained_directories) {
        // Creating this journal-identified private directory changes the parent
        // times, not the identity/mode or any preexisting public child.
        CK_CHECK(target_after.at(".").device == target_before.at(".").device &&
                 target_after.at(".").inode == target_before.at(".").inode &&
                 target_after.at(".").mode == target_before.at(".").mode);
        target_after.at(".") = target_before.at(".");
    }
    CK_CHECK(target_after == target_before);
    const auto retained_target = ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "right");
    const auto journals_before = ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "operation-journals");
    for (unsigned poll = 0; poll != 3; ++poll) fixture.application.step(fixture.clock.now_nanos());
    CK_CHECK(ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "operation-journals") == journals_before);
    CK_CHECK(ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "left") == source_before);
    CK_CHECK(ck::testing::observe_files_fixture_tree(fixture.scratch.path() / "right") == retained_target);
}
#endif
} // namespace
CK_TEST(files_app_composes_independent_native_panes_in_every_suite_theme) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        CK_CHECK(f.app->model(0).row_count() == 100 && f.app->model(1).row_count() == 3);
        CK_CHECK(f.app->target(0) && f.app->target(1) && !f.app->narrow());
        const auto text = f.painted();
        CK_CHECK(text.find("left-item-") != text.npos && text.find("right-item-") != text.npos);
        CK_CHECK(text.find("[Active] Left") != text.npos && text.find("[Other] Right") != text.npos);
        CK_CHECK(text.find("Active: Left | Items: L 100 / R 3 | Marked: L 0 / R 0 | Ready") != text.npos);
        CK_CHECK(f.app->status(0) == "Last scan: 100 items");
    }
}
CK_TEST(files_app_status_reports_contextual_reason_for_empty_listing) {
    Fixture f;
    f.loaded();
    const auto empty = f.scratch.path() / "empty";
    std::filesystem::create_directory(empty);
    CK_CHECK(f.app->navigate(0, address(empty)));
    f.loaded();
    CK_CHECK(f.app->status(0).find("Hint: Enter folder unavailable: select a folder") != std::string::npos);
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.enter_directory")));
}
CK_TEST(files_app_status_reports_operation_reason_for_selected_row_when_other_pane_is_busy) {
    auto gate = std::make_shared<Gate>();
    Fixture f(ThemeScheme::Classic, gate);
    gate->wait();
    f.until([&] { return f.app->target(1).has_value(); });
    f.app->switch_pane();
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->active_pane() == 1 && f.app->target(1));
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.copy")));
    CK_CHECK(f.app->availability("ck.files.copy").reason ==
             "Copy unavailable: both panes must be ready");
    CK_CHECK(f.app->status(1).find("Hint: Copy unavailable: both panes must be ready") != std::string::npos);
    gate->release();
    f.loaded();
}
CK_TEST(files_app_trash_refuses_directory_until_recursive_identity_worker_is_qualified) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->model(0).cursor_kind() == ck::files::ObjectKind::Directory);
    const auto reason = "Trash / Recycle Bin unavailable: recursive identity-bound Trash is not qualified";
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.trash")));
    CK_CHECK(f.app->availability("ck.files.trash").reason == reason);
    CK_CHECK(f.app->status(0).find("Hint: Trash / Recycle Bin unavailable: recursive identity-bound Trash is not qualified") !=
             std::string::npos);
    CK_CHECK(!f.command("ck.files.trash"));
    CK_CHECK(std::filesystem::is_directory(f.scratch.path() / "left" / "left-item-0"));
    CK_CHECK(std::filesystem::exists(f.scratch.path() / "left" / "left-item-0" / "inside.txt"));
}
CK_TEST(files_app_narrow_layout_switches_without_discarding_either_pane) {
    Fixture f;
    f.loaded();
    const auto left = f.app->target(0)->row.row;
    const auto right = f.app->target(1)->row.row;
    f.terminal.resize({80, 24});
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->narrow() && f.app->grid(0)->bounds().width > 70);
    CK_CHECK(f.application.execute_command(f.app->switch_command()));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->active_pane() == 1 && f.application.focused() == f.app->grid(1));
    CK_CHECK(f.app->target(0)->row.row == left && f.app->target(1)->row.row == right);
    const auto text = f.painted();
    CK_CHECK(text.find("[Active] Right") != text.npos && text.find("Active: Right") != text.npos);
    f.terminal.resize({120, 34});
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->narrow() && f.app->active_pane() == 1);
}
CK_TEST(files_app_failed_navigation_retains_previous_rows_and_reports_failure) {
    Fixture f;
    f.loaded();
    const auto old = f.app->model(0).cell(0, 1).text;
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "missing")));
    CK_CHECK(!f.app->target(0));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 100 && f.app->model(0).cell(0, 1).text == old);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
    CK_CHECK(f.app->model(1).row_count() == 3 && f.app->target(1));
}
CK_TEST(files_app_keyboard_pages_beyond_initial_cache_and_keeps_other_cursor_independent) {
    Fixture f;
    f.loaded();
    const auto other = f.app->target(1)->row.row;
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::End, ckv::Modifier::Ctrl, {}}}));
    CK_CHECK(f.app->model(0).cursor().row == 99 && !f.app->target(0));
    f.until([&] { return !f.app->pending(0) && f.app->target(0).has_value(); });
    CK_CHECK(f.app->model(0).cell(99, 1).text != "Loading...");
    CK_CHECK(f.app->target(1)->row.row == other && f.app->model(1).cursor().row == 0);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::F4, ckv::Modifier::None, {}}}));
    CK_CHECK(f.app->active_pane() == 1 && f.application.focused() == f.app->grid(1));
    CK_CHECK(f.app->model(0).cursor().row == 99);
}
CK_TEST(files_app_rapid_navigation_publishes_only_the_latest_requested_location) {
    Fixture f;
    for (int i = 0; i < 20; ++i)
        CK_CHECK(f.app->navigate(0, address(f.scratch.path() / (i % 2 == 0 ? "missing" : "left"))));
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 3 && f.app->target(0));
    CK_CHECK(f.app->model(0).cell(0, 1).text == "right-item-0");
    CK_CHECK(f.app->status(0) == "Last scan: 3 items");
}
CK_TEST(files_app_mark_controls_acknowledge_changes_and_preserve_the_other_pane) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.key(ckv::Key::Insert));
    CK_CHECK(f.app->pending(0) && !f.app->target(0));
    CK_CHECK(!f.command("ck.files.toggle_mark"));
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(0).cursor().row == 0); // Input pauses while marks commit.
    CK_CHECK(f.command("ck.files.switch_pane") && f.app->active_pane() == 1);
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(1).cursor().row == 1);
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->model(0).cursor_marked() == true);
    CK_CHECK(f.app->selection(1).marks->total == 0 && f.app->status(0).find("Marked: 1") != std::string::npos);
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->selection(1).marks->total == 3);
    CK_CHECK(f.command("ck.files.invert_marks"));
    f.loaded();
    CK_CHECK(f.app->selection(1).marks->total == 0 && f.app->selection(0).marks->total == 1);
}
CK_TEST(files_app_range_marks_span_uncached_rows_and_are_reset_by_a_view_change) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start"));
    CK_CHECK(f.key(ckv::Key::End, ckv::Modifier::Ctrl));
    f.until([&] { return !f.app->pending(0) && f.app->target(0); });
    CK_CHECK(f.command("ck.files.mark_range"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 100 && f.app->model(0).cursor_marked() == true);
    CK_CHECK(f.command("ck.files.unmark_range"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 0);
    auto view = f.app->view(0);
    view.descending = true;
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(!f.command("ck.files.mark_range") && f.app->status(0).find("Range start set") == std::string::npos);
}
CK_TEST(files_app_view_change_acknowledges_current_row_and_keeps_focus_and_marks_through_reorder) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        if (theme == ThemeScheme::Classic) {
            const auto cell = f.app->grid(0)->cell_rect({7, 1});
            if (!cell)
                throw std::runtime_error("missing real pane row cell");
            const auto grid = f.app->grid(0)->absolute_bounds();
            const ckv::Point at{grid.x + cell->x + cell->width / 2, grid.y + cell->y};
            CK_CHECK(f.application.dispatch(
                ckv::MouseEvent{.action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = at}));
            CK_CHECK(f.application.dispatch(
                ckv::MouseEvent{.action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = at}));
        } else
            for (unsigned i = 0; i < 7; ++i)
                CK_CHECK(f.key(ckv::Key::Down));
        const auto target = f.app->target(0)->row.row;
        CK_CHECK(f.command("ck.files.toggle_mark"));
        f.loaded();
        auto view = f.app->view(0);
        view.descending = true;
        CK_CHECK(f.app->change_view(0, view));
        CK_CHECK(f.app->pending(0) && !f.app->change_view(0, view) && !f.app->target(0));
        f.key(ckv::Key::Down);
        CK_CHECK(f.app->model(0).cursor().row == 7);
        f.loaded();
        CK_CHECK(f.app->view(0).descending && f.app->target(0)->row.row == target);
        CK_CHECK(f.app->model(0).cursor().row == 92 && f.app->model(0).cursor_marked() == true);
        CK_CHECK(f.app->selection(0).marks->total == 1 && !f.app->view(1).descending);
    }
}
CK_TEST(files_app_filtering_keeps_hidden_marks_explicit_and_clear_all_removes_them) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    auto view = f.app->view(0);
    view.display_substring = "left-item-0";
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 1 && f.app->selection(0).marks->hidden == 99);
    CK_CHECK(f.app->status(0).find("99 hidden; review required") != std::string::npos);
    CK_CHECK(f.command("ck.files.unmark_visible"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 99 && !*f.app->model(0).cursor_marked());
    CK_CHECK(f.command("ck.files.clear_marks"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.clear_marks"));
    view.display_substring = "no item matches";
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 0 && !f.command("ck.files.toggle_mark") &&
             !f.command("ck.files.mark_visible"));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    view.display_substring.clear();
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 100 && f.app->target(0));
}
CK_TEST(files_app_pane_commands_do_not_run_from_unrelated_input_or_view_options) {
    Fixture f;
    f.loaded();
    auto input = std::make_unique<ckv::widgets::InputLine>();
    input->set_bounds({0, 0, 20, 1});
    auto *outside = f.application.root().add_child(std::move(input));
    f.application.set_focus(outside);
    CK_CHECK(!f.command("ck.files.toggle_mark") && !f.command("ck.files.view_options"));
    CK_CHECK(!f.application.command_available(f.app->switch_command()));
    f.application.root().remove_child(outside);
    f.application.set_focus(f.app->grid(0));
    CK_CHECK(f.command("ck.files.view_options") && f.app->view_options_pending());
    CK_CHECK(!f.command("ck.files.toggle_mark") && !f.command("ck.files.mark_visible"));
    CK_CHECK(!f.command("ck.files.switch_pane"));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->view_options_pending() && !f.app->pending(0) && f.app->selection(0).marks->total == 0);
}
CK_TEST(files_app_refresh_is_an_async_reopen_and_properties_use_acknowledged_row_metadata) {
    Fixture f;
    f.loaded();
    const auto before = f.app->location(0);
    const auto history = f.app->history_size(0);
    CK_CHECK(f.command("ck.files.refresh"));
    CK_CHECK(f.app->pending(0) && f.app->history_size(0) == history);
    f.loaded();
    CK_CHECK(f.app->location(0) && before && *f.app->location(0) == *before && f.app->history_size(0) == history);
    CK_CHECK(f.command("ck.files.properties") && f.app->properties_pending());
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->properties_pending());
}
CK_TEST(files_app_new_folder_uses_strict_dialog_worker_mutation_and_refreshes_listing) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.create_directory"));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(inputs.size() == 1);
    CK_CHECK(buttons.size() == 2);
    inputs.front()->set_text("created-by-app");
    f.application.set_focus(inputs.front());
    ckv::widgets::Button *create = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Create")
            create = button;
    CK_CHECK(create != nullptr && create->activate_mnemonic("c"));
    f.until([&] { return !f.app->pending(0); });

    CK_CHECK(std::filesystem::is_directory(f.scratch.path() / "left" / "created-by-app"));
    CK_CHECK(f.app->model(0).row_count() == 101);
    CK_CHECK(f.app->status(0).find("Last scan: 101 items") != std::string::npos);
    CK_CHECK(f.app->status(0).find("Hint: Trash / Recycle Bin unavailable: recursive identity-bound Trash is not qualified") !=
             std::string::npos);
}
CK_TEST(files_app_new_file_uses_strict_dialog_worker_mutation_and_refreshes_listing) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.create_file"));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(inputs.size() == 1);
    CK_CHECK(buttons.size() == 2);
    inputs.front()->set_text("created-by-app.txt");
    f.application.set_focus(inputs.front());
    ckv::widgets::Button *create = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Create")
            create = button;
    CK_CHECK(create != nullptr && create->activate_mnemonic("c"));
    f.until([&] { return !f.app->pending(0); });

    CK_CHECK(std::filesystem::is_regular_file(f.scratch.path() / "left" / "created-by-app.txt"));
    CK_CHECK(std::filesystem::file_size(f.scratch.path() / "left" / "created-by-app.txt") == 0);
    CK_CHECK(f.app->model(0).row_count() == 101);
    CK_CHECK(f.app->status(0).find("Last scan: 101 items") != std::string::npos);
}
CK_TEST(files_app_new_hard_link_uses_strict_dialog_worker_mutation_and_refreshes_listing) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.create_hard_link"));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(inputs.size() == 1);
    CK_CHECK(buttons.size() == 2);
    inputs.front()->set_text("hard-link-by-app");
    f.application.set_focus(inputs.front());
    ckv::widgets::Button *create = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Create")
            create = button;
    CK_CHECK(create != nullptr && create->activate_mnemonic("c"));
    f.until([&] { return !f.app->pending(0); });

    CK_CHECK(std::filesystem::is_regular_file(f.scratch.path() / "left" / "hard-link-by-app"));
    CK_CHECK(std::filesystem::hard_link_count(f.scratch.path() / "left" / "left-item-0") == 2);
    CK_CHECK(f.app->model(0).row_count() == 101);
    CK_CHECK(f.app->status(0).find("Last scan: 101 items") != std::string::npos);
}
CK_TEST(files_app_symbolic_link_command_is_capability_scoped_and_uses_opaque_target_dialog) {
    Fixture f;
    f.loaded();
#if defined(_WIN32)
    CK_CHECK(!f.command("ck.files.create_symbolic_link"));
    CK_CHECK(f.app->availability("ck.files.create_symbolic_link").reason ==
             "Symbolic link unavailable: handle-relative Windows reparse publication is not qualified");
#else
    CK_CHECK(f.command("ck.files.create_symbolic_link"));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(inputs.size() == 2);
    CK_CHECK(buttons.size() == 2);
    inputs[0]->set_text("symbolic-link-by-app");
    inputs[1]->set_text("../target");
    f.application.set_focus(inputs[1]);
    ckv::widgets::Button *create = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Create")
            create = button;
    CK_CHECK(create != nullptr && create->activate_mnemonic("c"));
    f.until([&] { return !f.app->pending(0); });

    CK_CHECK(std::filesystem::is_symlink(f.scratch.path() / "left" / "symbolic-link-by-app"));
    CK_CHECK(std::filesystem::read_symlink(f.scratch.path() / "left" / "symbolic-link-by-app") ==
             std::filesystem::path("../target"));
    CK_CHECK(f.app->model(0).row_count() == 101);
#endif
}
CK_TEST(files_app_link_inspection_is_disabled_for_a_regular_file_with_status_reason) {
    Fixture f;
    f.loaded();
    CK_CHECK(!f.command("ck.files.inspect_link"));
    CK_CHECK(f.app->availability("ck.files.inspect_link").reason ==
             "Link inspection unavailable: select a symbolic link or reparse point");
}
CK_TEST(files_app_rename_uses_strict_dialog_worker_mutation_and_refreshes_listing) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.rename"));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(inputs.size() == 1);
    CK_CHECK(buttons.size() == 2);
    inputs.front()->set_text("renamed-by-app");
    f.application.set_focus(inputs.front());
    ckv::widgets::Button *rename = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Rename")
            rename = button;
    CK_CHECK(rename != nullptr && rename->activate_mnemonic("r"));
    f.until([&] { return !f.app->pending(0); });

    CK_CHECK(!std::filesystem::exists(f.scratch.path() / "left" / "left-item-0"));
    CK_CHECK(std::filesystem::is_regular_file(f.scratch.path() / "left" / "renamed-by-app"));
    CK_CHECK(f.app->model(0).row_count() == 100);
    CK_CHECK(f.app->status(0) == "Last scan: 100 items");
}
CK_TEST(files_app_permanent_delete_requires_explicit_confirmation_and_cancel_preserves_entry) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.permanently_delete"));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), buttons);
    CK_CHECK(buttons.size() == 2);
    ckv::widgets::Button *cancel = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Cancel")
            cancel = button;
    CK_CHECK(cancel != nullptr && f.app->status(0) == "Last scan: 100 items");
    CK_CHECK(cancel->activate_mnemonic("c"));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(std::filesystem::is_regular_file(f.scratch.path() / "left" / "left-item-0"));
    CK_CHECK(!f.app->pending(0) && f.app->model(0).row_count() == 100);
}
CK_TEST(files_app_copy_command_plans_through_review_before_execution) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.copy"));
    f.until([&] { return f.app->operation_review_pending(); });
    CK_CHECK(f.app->operation_review_pending());
    CK_CHECK(!f.command("ck.files.rename") && !f.command("ck.files.create_directory"));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->operation_review_pending());
    CK_CHECK(!std::filesystem::exists(f.scratch.path() / "right" / "left-item-0"));
}
#if defined(__APPLE__)
CK_TEST(files_app_copy_acceptance_reports_unqualified_mac_publication_after_review)
#else
CK_TEST(files_app_copy_acceptance_executes_after_review_and_refreshes_both_panes)
#endif
{
    Fixture f;
    f.loaded();
#if defined(__APPLE__)
    const auto source_before = ck::testing::observe_files_fixture_tree(f.scratch.path() / "left");
    const auto target_before = ck::testing::observe_files_fixture_tree(f.scratch.path() / "right");
#endif
    CK_CHECK(f.command("ck.files.copy"));
    f.until([&] { return f.app->operation_review_pending(); });
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::CheckGroup *> checks;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), checks);
    find_views(f.application.root(), buttons);
    CK_CHECK(combos.size() == 1);
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply decision")
            apply = button;
    CK_CHECK(apply != nullptr);
    // Every displayed preservation row must be acknowledged before continuing.
    for (auto *check : checks)
        check->set_checked(0, true);
    // The copy adapter declares an explicit portable-metadata boundary; the
    // action order is Cancel, choose destination, continue, skip.
    combos.front()->set_selected_index(2);
    CK_CHECK(apply->activate_mnemonic("a"));
#if defined(__APPLE__)
    expect_mac_copy_publication_refusal(f, source_before, target_before);
#else
    f.until([&] { return std::filesystem::exists(f.scratch.path() / "right" / "left-item-0") &&
                        !f.app->pending(0) && !f.app->pending(1) &&
                        f.app->status(0).find("Copying") == std::string::npos &&
                        f.app->status(1).find("Copying") == std::string::npos; });
    CK_CHECK(f.app->status(0).find("Last scan:") != std::string::npos);
    CK_CHECK(f.app->status(1).find("Last scan:") != std::string::npos);
#endif
}
CK_TEST(files_app_move_acceptance_executes_after_review_and_refreshes_both_panes) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.move"));
    f.until([&] { return f.app->operation_review_pending(); });
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::CheckGroup *> checks;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), checks);
    find_views(f.application.root(), buttons);
    CK_CHECK(combos.size() == 1);
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply decision")
            apply = button;
    CK_CHECK(apply != nullptr);
    for (auto *check : checks)
        check->set_checked(0, true);
    combos.front()->set_selected_index(2);
    CK_CHECK(apply->activate_mnemonic("a"));
    f.until([&] {
        return !std::filesystem::exists(f.scratch.path() / "left" / "left-item-0") &&
               std::filesystem::exists(f.scratch.path() / "right" / "left-item-0") && !f.app->pending(0) &&
               !f.app->pending(1) && f.app->status(0).find("Moving") == std::string::npos &&
               f.app->status(1).find("Moving") == std::string::npos;
    });
    CK_CHECK(f.app->status(0).find("Last scan:") != std::string::npos);
    CK_CHECK(f.app->status(1).find("Last scan:") != std::string::npos);
}
#if defined(__APPLE__)
CK_TEST(files_app_recursive_copy_acceptance_reports_unqualified_mac_publication)
#else
CK_TEST(files_app_copy_acceptance_executes_recursive_directory_after_review)
#endif
{
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
#if defined(__APPLE__)
    const auto source_before = ck::testing::observe_files_fixture_tree(f.scratch.path() / "left");
    const auto target_before = ck::testing::observe_files_fixture_tree(f.scratch.path() / "right");
#endif
    for (std::size_t row = 0; row != f.app->model(0).row_count() &&
         f.app->model(0).cursor_kind() != ObjectKind::Directory; ++row) {
        CK_CHECK(f.key(ckv::Key::Down));
        f.loaded();
    }
    CK_CHECK(f.app->model(0).cursor_kind() == ObjectKind::Directory);
    CK_CHECK(f.command("ck.files.copy"));
    f.until([&] { return f.app->operation_review_pending(); });
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::CheckGroup *> checks;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), checks);
    find_views(f.application.root(), buttons);
    CK_CHECK(combos.size() == 1 && !checks.empty());
    for (auto *check : checks) {
        const auto rows = static_cast<std::size_t>(check->vertical_size_hint().preferred);
        for (std::size_t index = 0; index != rows; ++index)
            check->set_checked(index, true);
    }
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply decision")
            apply = button;
    CK_CHECK(apply != nullptr);
    combos.front()->set_selected_index(2); // Continue with listed losses.
    CK_CHECK(apply->activate_mnemonic("a"));
#if defined(__APPLE__)
    expect_mac_copy_publication_refusal(f, source_before, target_before);
#else
    f.until([&] {
        return std::filesystem::is_regular_file(f.scratch.path() / "right" / "left-item-0" / "inside.txt") &&
               std::filesystem::is_regular_file(f.scratch.path() / "right" / "left-item-0" / "nested" / "leaf.txt") &&
               !f.app->pending(0) && !f.app->pending(1) &&
               f.app->status(0).find("Copying") == std::string::npos &&
               f.app->status(1).find("Copying") == std::string::npos;
    });
    CK_CHECK(f.app->status(0).find("Last scan:") != std::string::npos);
    CK_CHECK(f.app->status(1).find("Last scan:") != std::string::npos);
#endif
}
#if defined(__APPLE__)
CK_TEST(files_app_marked_copy_acceptance_preserves_inputs_after_mac_reconciliation)
#else
CK_TEST(files_app_copy_acceptance_copies_the_acknowledged_marked_batch)
#endif
{
    Fixture f;
    f.loaded();
#if defined(__APPLE__)
    const auto source_before = ck::testing::observe_files_fixture_tree(f.scratch.path() / "left");
    const auto target_before = ck::testing::observe_files_fixture_tree(f.scratch.path() / "right");
#endif
    CK_CHECK(f.key(ckv::Key::Insert));
    f.loaded();
    CK_CHECK(f.key(ckv::Key::Down));
    f.loaded();
    CK_CHECK(f.key(ckv::Key::Insert));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 2);
    CK_CHECK(f.command("ck.files.copy"));
    f.until([&] { return f.app->operation_review_pending(); });
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::CheckGroup *> checks;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), checks);
    find_views(f.application.root(), buttons);
    // The portable copy boundary contributes the required rows; native
    // platforms may disclose additional preservation limits.
    CK_CHECK(combos.size() == 1 && checks.size() >= 2);
    for (auto *check : checks)
        check->set_checked(0, true);
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply decision")
            apply = button;
    CK_CHECK(apply != nullptr);
    combos.front()->set_selected_index(2); // Continue with listed losses.
    CK_CHECK(apply->activate_mnemonic("a"));
#if defined(__APPLE__)
    expect_mac_copy_publication_refusal(f, source_before, target_before, true);
#else
    f.until([&] {
        return std::filesystem::exists(f.scratch.path() / "right" / "left-item-0") &&
               std::filesystem::exists(f.scratch.path() / "right" / "left-item-1") &&
               !f.app->pending(0) && !f.app->pending(1) &&
               f.app->status(0).find("Copying") == std::string::npos &&
               f.app->status(1).find("Copying") == std::string::npos;
    });
    CK_CHECK(f.app->status(0).find("Last scan:") != std::string::npos);
    CK_CHECK(f.app->status(1).find("Last scan:") != std::string::npos);
#endif
}
CK_TEST(files_app_rename_uses_stamped_worker_target_on_all_platforms) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.key(ckv::Key::F2));
    f.application.step(f.clock.now_nanos());

    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(inputs.size() == 1);
    CK_CHECK(buttons.size() == 2);
    inputs.front()->set_text("renamed-by-app");
    f.application.set_focus(inputs.front());
    ckv::widgets::Button *rename = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Rename")
            rename = button;
    CK_CHECK(rename != nullptr && rename->activate_mnemonic("r"));
    f.until([&] { return !f.app->pending(0); });

#if defined(_WIN32)
    CK_CHECK(std::filesystem::exists(f.scratch.path() / "left" / "renamed-by-app"));
    CK_CHECK(!std::filesystem::exists(f.scratch.path() / "left" / "left-item-0"));
#else
    CK_CHECK(std::filesystem::exists(f.scratch.path() / "left" / "renamed-by-app"));
    CK_CHECK(!std::filesystem::exists(f.scratch.path() / "left" / "left-item-0"));
#endif
}
CK_TEST(files_app_preview_reads_only_a_bounded_exact_row_and_returns_to_idle) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.app->model(0).cursor_kind() == ObjectKind::RegularFile);
    CK_CHECK(f.command("ck.files.preview"));
    CK_CHECK(f.app->pending(0) && f.app->preview_pending());
    f.until([&] { return !f.app->pending(0) && f.app->preview_pending(); });
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->preview_pending());
}
CK_TEST(files_app_view_options_materialize_typed_controls_and_accept_via_mouse_after_narrow_resize) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.view_options"));
    f.terminal.resize({80, 24});
    f.application.step(f.clock.now_nanos());
    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::CheckGroup *> checks;
    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), checks);
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(combos.size() == 1 && checks.size() == 4);
    combos.at(0)->set_selected_index(2); // Bytes, not a parsed display string.
    checks.at(0)->set_checked(0, true);
    checks.at(1)->set_checked(0, false);
    checks.at(2)->set_checked(0, true);
    checks.at(3)->set_checked(0, true);
    ckv::widgets::InputLine *filter = nullptr;
    for (auto *input : inputs) {
        bool in_combo = false;
        for (auto *parent = input->parent(); parent; parent = parent->parent())
            in_combo = in_combo || dynamic_cast<ckv::widgets::ComboBox *>(parent) != nullptr;
        if (!in_combo)
            filter = input;
    }
    if (!filter)
        throw std::runtime_error("missing real view filter input");
    filter->set_text("left-item-0");
    f.application.set_focus(filter);
    f.application.step(f.clock.now_nanos());
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply")
            apply = button;
    if (!apply)
        throw std::runtime_error("missing real view Apply button");
    const auto rect = apply->absolute_bounds();
    CK_CHECK(rect.width > 0 && rect.height > 0 && rect.y >= 0 && rect.y < 24 && rect.x >= 0 && rect.x < 80);
    const ckv::Point click{rect.x, rect.y};
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = click}));
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = click}));
    f.until([&] { return !f.app->view_options_pending() && !f.app->pending(0); });
    const auto &view = f.app->view(0);
    CK_CHECK(view.sort == PaneSortKey::Size && view.descending && !view.folders_first && view.show_hidden &&
             view.ascii_case_sensitive);
    CK_CHECK(view.display_substring == "left-item-0" && f.app->model(0).row_count() == 1);
}
CK_TEST(files_app_marks_menu_routes_context_commands_and_quit_cancels_view_options) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "m"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "m"}}));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 100);
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    f.until([&] { return f.exits == 1; });
    CK_CHECK(!f.app->view_options_pending() && !f.app->close_confirmation_pending());
    CK_CHECK(f.app->view(0).display_substring.empty());
}
CK_TEST(files_app_view_dialog_rejects_oversize_filters_and_obsolete_navigation_contexts) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.command("ck.files.view_options"));
    std::vector<ckv::widgets::InputLine *> inputs;
    find_views(f.application.root(), inputs);
    ckv::widgets::InputLine *filter = nullptr;
    for (auto *input : inputs) {
        bool in_combo = false;
        for (auto *parent = input->parent(); parent; parent = parent->parent())
            in_combo = in_combo || dynamic_cast<ckv::widgets::ComboBox *>(parent) != nullptr;
        if (!in_combo)
            filter = input;
    }
    if (!filter)
        throw std::runtime_error("missing filter field");
    filter->set_text(std::string(4097, 'x'));
    f.application.set_focus(filter);
    CK_CHECK(f.key(ckv::Key::Enter));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.app->view_options_pending() && !f.app->pending(0));
    filter->set_text("right-item-0");
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.loaded();
    f.application.set_focus(filter);
    CK_CHECK(f.key(ckv::Key::Enter));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->view_options_pending() && !f.app->pending(0));
    CK_CHECK(f.app->model(0).row_count() == 3 && f.app->view(0).display_substring.empty());
    CK_CHECK(f.app->status(0).find("Location changed while view options were open") != std::string::npos);
}
CK_TEST(files_app_reports_committed_marks_when_display_capture_fails_and_recovers_after_resize) {
    FilesPaneServiceLimits limits;
    limits.page_wire_bytes = 4096;
    Fixture f(ThemeScheme::Classic, {}, limits, {120, 12});
    f.loaded();
    CK_CHECK(f.app->target(0));
    f.terminal.resize({120, 60});
    f.application.step(f.clock.now_nanos());
    f.loaded();
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->status(0).find("Change applied;") != std::string::npos && !f.app->target(0));
    CK_CHECK(!f.command("ck.files.toggle_mark"));
    f.terminal.resize({120, 12});
    f.application.step(f.clock.now_nanos());
    f.until([&] { return !f.app->pending(0) && f.app->target(0); });
    CK_CHECK(f.app->selection(0).marks->total == 100 && f.app->model(0).cursor_marked() == true);
    CK_CHECK(f.app->status(0).find("Change applied;") != std::string::npos);
}
CK_TEST(files_app_cancelling_an_acknowledged_focus_stage_does_not_start_its_queued_view) {
    Fixture f;
    f.loaded();
    for (unsigned i = 0; i < 7; ++i)
        f.key(ckv::Key::Down);
    auto view = f.app->view(0);
    view.descending = true;
    CK_CHECK(f.app->change_view(0, view));
    // Keep the UI's focus acknowledgment unread: cancel() must also withdraw
    // the queued view when the worker has already finished that first stage.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.services[0]->working()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("focus preparation did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(f.command("ck.files.cancel_read"));
    f.loaded();
    CK_CHECK(!f.app->view(0).descending && f.app->model(0).cursor().row == 7);
    CK_CHECK(f.app->status(0).find("View change cancelled before application") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(0).cursor().row == 8);
}
CK_TEST(files_app_unknown_change_acknowledgment_blocks_that_pane_and_keeps_the_other_usable) {
    auto fault = std::make_shared<std::atomic<bool>>(false);
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, fault);
    f.loaded();
    *fault = true;
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->status(0).find("Change outcome unknown;") != std::string::npos);
    CK_CHECK(!f.app->target(0) && !f.command("ck.files.toggle_mark") && !f.command("ck.files.view_options"));
    CK_CHECK(f.command("ck.files.switch_pane") && f.app->active_pane() == 1);
    CK_CHECK(f.command("ck.files.mark_visible"));
    f.loaded();
    CK_CHECK(f.app->selection(1).marks->total == 3 && f.app->target(1));
}
CK_TEST(files_app_view_changes_resolve_an_offscreen_uncached_cursor_without_inventing_row_identity) {
    for (bool move_cursor : {false, true}) {
        Fixture f;
        f.loaded();
        if (move_cursor)
            for (unsigned i = 0; i < 7; ++i)
                f.key(ckv::Key::Down);
        const auto selected = f.app->target(0)->row.row;
        const auto rect = f.app->grid(0)->absolute_bounds();
        for (unsigned i = 0; i < 13; ++i)
            CK_CHECK(f.application.dispatch(ckv::MouseEvent{.action = ckv::MouseAction::Wheel,
                                                            .button = ckv::MouseButton::WheelDown,
                                                            .cell = {rect.x + 5, rect.y + 2}}));
        f.loaded();
        CK_CHECK(f.app->model(0).viewport().first > 20 && !f.app->model(0).target());
        auto view = f.app->view(0);
        view.descending = true;
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.app->view(0).descending && f.app->target(0)->row.row == selected);
        CK_CHECK(f.app->model(0).cursor().row == (move_cursor ? 92 : 99));
    }
}
CK_TEST(files_app_idle_quit_stops_services_and_restores_host_handler) {
    Fixture f;
    f.loaded();
    f.until([&] { return !f.services[0]->working() && !f.services[1]->working(); });
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    CK_CHECK(f.exits == 1 && !f.app->close_confirmation_pending() && !f.app->target(0));
    f.app.reset();
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    CK_CHECK(f.exits == 2);
}
CK_TEST(files_app_blocked_read_keeps_other_pane_live_and_close_requires_explicit_exit_choice) {
    auto gate = std::make_shared<Gate>();
    Fixture f(ThemeScheme::Classic, gate);
    gate->wait();
    f.until([&] { return !f.app->pending(1); });
    CK_CHECK(f.app->pending(0) && f.app->target(1) && !f.app->target(0));
    CK_CHECK(f.application.execute_command(f.app->switch_command()));
    CK_CHECK(f.app->active_pane() == 1);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Down, ckv::Modifier::None, {}}}));
    CK_CHECK(f.app->model(1).cursor().row == 1);
    CK_CHECK(f.application.execute_command(f.application.commands().standard().quit));
    CK_CHECK(f.app->close_confirmation_pending() && f.exits == 0);
    CK_CHECK(!f.application.execute_command(f.app->switch_command()));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Enter, ckv::Modifier::None, {}}}));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->close_confirmation_pending() && f.exits == 0 && f.services[0]->running());
    f.app->request_close();
    CK_CHECK(f.app->close_confirmation_pending());
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Escape, ckv::Modifier::None, {}}}));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->close_confirmation_pending() && f.exits == 0);
    f.app->request_close();
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "x"}}));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.exits == 1 && f.services[0]->running()); // No join inside dispatch.
    CK_CHECK(!f.app->target(1));
    gate->release();
}
CK_TEST(files_app_cancellation_waits_for_blocked_storage_without_blocking_dispatch) {
    auto gate = std::make_shared<Gate>();
    Fixture f(ThemeScheme::Classic, gate);
    gate->wait();
    CK_CHECK(f.application.execute_command(f.app->cancel_command()));
    CK_CHECK(f.app->pending(0) && !f.app->target(0));
    f.until([&] { return !f.app->pending(1); });
    CK_CHECK(f.app->target(1));
    gate->release();
    f.loaded();
    CK_CHECK(f.app->model(0).row_count() == 0 && !f.app->target(0));
    CK_CHECK(f.app->status(0).find("cancelled") != std::string::npos);
}
CK_TEST(files_app_wake_coalesces_worker_notifications_and_ignores_late_callbacks) {
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal({80, 24});
    ckv::ui::Application application(terminal, clock);
    unsigned calls = 0;
    std::function<void()> late;
    {
        FilesAppWake wake(application);
        wake.bind([&] { ++calls; });
        late = wake.callback();
        std::thread worker([&] {
            for (unsigned i = 0; i < 10000; ++i)
                late();
        });
        worker.join();
        CK_CHECK(calls == 0);
        application.step(0);
        CK_CHECK(calls == 1);
        late();
        wake.unbind();
        application.step(0);
        CK_CHECK(calls == 1);
    }
    late();
    application.step(0);
    CK_CHECK(calls == 1);
}

CK_TEST(files_app_enter_folder_preserves_view_policy_clears_filter_and_commits_only_after_acknowledgment) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme, {}, {}, {120, 34}, {}, true);
        f.loaded();
        auto view = f.app->view(0);
        view.descending = view.folders_first = view.show_hidden = view.ascii_case_sensitive = true;
        view.display_substring = "left-item-0";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
        f.loaded();
        const auto previous = f.app->location(0), other = f.app->location(1);
        const auto other_target = f.app->target(1)->row.row;
        CK_CHECK(f.key(ckv::Key::Enter));
        CK_CHECK(f.app->pending(0) && !f.app->target(0) && f.app->location(0) == previous);
        f.loaded();
        CK_CHECK(f.app->location(0)->root_address() == previous->root_address());
        CK_CHECK(f.app->location(0)->components().size() == 1 &&
                 f.app->location(0)->components()[0].display_name() == "left-item-0");
        CK_CHECK(f.app->model(0).row_count() == 2 && f.app->view(0).display_substring.empty());
        CK_CHECK(f.app->view(0).descending && f.app->view(0).folders_first && f.app->view(0).show_hidden &&
                 f.app->view(0).ascii_case_sensitive);
        CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.mark_range"));
        CK_CHECK(f.app->location(1) == other && f.app->target(1)->row.row == other_target);
        const auto text = f.painted();
        CK_CHECK(text.find("nested") != text.npos && text.find("inside.txt") != text.npos);
        CK_CHECK(f.app->status(0).find("Range start set") == std::string::npos);
    }
}
CK_TEST(files_app_folder_double_click_routes_to_the_clicked_pane_and_keeps_the_other_live) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    const auto left = f.app->location(0);
    const auto rect = f.app->grid(1)->cell_rect({0, 1});
    if (!rect)
        throw std::runtime_error("missing folder cell");
    const auto grid = f.app->grid(1)->absolute_bounds();
    const ckv::Point at{grid.x + rect->x + rect->width / 2, grid.y + rect->y};
    for (int click : {1, 2}) {
        CK_CHECK(f.application.dispatch(ckv::MouseEvent{
            .action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = at, .click_count = click}));
        (void)f.application.dispatch(ckv::MouseEvent{
            .action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = at, .click_count = click});
    }
    CK_CHECK(f.app->active_pane() == 1 && f.app->pending(1));
    f.loaded();
    CK_CHECK(f.app->location(1)->components().size() == 1 &&
             f.app->location(1)->components()[0].display_name() == "right-item-0");
    CK_CHECK(f.app->location(0) == left && f.app->model(0).row_count() == 100 && f.app->target(0));
}
CK_TEST(files_app_folder_keyboard_activation_uses_live_keymap_and_routes_F2_to_rename) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    f.application.commands().unbind_key({ckv::Key::Enter, ckv::Modifier::None, {}});
    CK_CHECK(!f.key(ckv::Key::Enter) && !f.app->pending(0));
    CK_CHECK(f.key(ckv::Key::F2) && !f.app->pending(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.enter_directory"));
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
}
CK_TEST(files_app_folder_menu_and_scope_do_not_enter_files_or_run_from_dialogs_and_unrelated_inputs) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.key(ckv::Key::Down));
    CK_CHECK(!f.command("ck.files.enter_directory") && !f.app->enter_directory(0));
    CK_CHECK(f.key(ckv::Key::Home));
    auto input = std::make_unique<ckv::widgets::InputLine>();
    input->set_bounds({0, 0, 20, 1});
    auto *outside = f.application.root().add_child(std::move(input));
    f.application.set_focus(outside);
    CK_CHECK(!f.command("ck.files.enter_directory"));
    f.application.root().remove_child(outside);
    f.application.set_focus(f.app->grid(0));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(!f.command("ck.files.enter_directory") && !f.app->enter_directory(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "e"}}));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
}
CK_TEST(files_app_failed_folder_entry_retains_location_rows_marks_range_and_cursor) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto location = f.app->location(0);
    const auto row = f.app->target(0)->row.row;
    std::filesystem::remove_all(f.scratch.path() / "left" / "left-item-0");
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(0) == location && f.app->model(0).row_count() == 100 && f.app->target(0)->row.row == row);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
    CK_CHECK(f.command("ck.files.mark_range"));
    f.loaded();
    CK_CHECK(f.app->selection(0).marks->total == 1);
}
CK_TEST(files_app_blocked_folder_entry_freezes_that_pane_and_cancels_without_retargeting_other_pane) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    const auto before = f.app->location(0);
    const auto cursor = f.app->model(0).cursor().row;
    *f.block_directory = true;
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.directory_gate->wait();
    CK_CHECK(f.app->pending(0) && !f.app->target(0) && f.app->status(0) == "Opening folder...");
    f.key(ckv::Key::Down);
    CK_CHECK(f.app->model(0).cursor().row == cursor);
    f.app->switch_pane();
    CK_CHECK(f.key(ckv::Key::Down) && f.app->model(1).cursor().row == 1 && f.app->target(1));
    f.app->switch_pane();
    CK_CHECK(f.command("ck.files.cancel_read"));
    f.directory_gate->release();
    f.loaded();
    CK_CHECK(f.app->location(0) == before && f.app->model(0).cursor().row == cursor && f.app->target(0));
    CK_CHECK(f.app->status(0).find("Read cancelled") != std::string::npos);
}
CK_TEST(files_app_folder_cancellation_after_commit_reports_the_opened_location) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.services[0]->working()) {
        if (std::chrono::steady_clock::now() >= until)
            throw std::runtime_error("folder worker did not finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    f.app->cancel_read();
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
    CK_CHECK(f.app->status(0).find("Location opened before cancellation completed") != std::string::npos);
}
CK_TEST(files_app_explicit_navigation_supersedes_blocked_folder_entry) {
    Fixture f(ThemeScheme::Classic, {}, {}, {80, 24}, {}, true);
    f.loaded();
    *f.block_directory = true;
    CK_CHECK(f.app->enter_directory(0));
    f.directory_gate->wait();
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.directory_gate->release();
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
             f.app->location(0)->components().empty());
    CK_CHECK(f.app->model(0).row_count() == 3 && f.app->narrow());
    CK_CHECK(f.painted().find("Active: Left") != std::string::npos);
}
CK_TEST(files_app_folder_notification_fault_blocks_only_that_pane_without_claiming_navigation_success) {
    auto fault = std::make_shared<std::atomic<bool>>(false);
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, fault, true);
    f.loaded();
    const auto previous = f.app->location(0);
    *fault = true;
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(!f.app->target(0) && !f.app->model(0).current() && f.app->location(0) == previous);
    CK_CHECK(!f.command("ck.files.enter_directory"));
    f.app->switch_pane();
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(1)->components().size() == 1);
}

CK_TEST(files_app_folder_entry_preserves_native_names_without_parsing_display_or_flattening_windows_paths) {
    Fixture f;
    f.loaded();
    const auto root = f.scratch.path() / "left";
#ifdef _WIN32
    auto preferred = root;
    preferred.make_preferred();
    auto native = preferred.native();
    if (!native.starts_with(L"\\\\?\\"))
        native = L"\\\\?\\" + native;
    const auto folder = std::filesystem::path(native + L"\\00-literal. ");
    const auto expected = *NativeName::from_windows(u"00-literal. ");
#else
    const auto folder = root / "00-escaped\\name\n";
    const auto expected = *NativeName::from_posix(folder.filename().native());
#endif
    std::filesystem::create_directory(folder);
    {
        std::ofstream file(folder / "leaf.txt");
        file << "leaf";
        if (!file)
            throw std::runtime_error("literal folder fixture write");
    }
    CK_CHECK(f.app->navigate(0, address(root)));
    f.loaded();
    CK_CHECK(f.app->model(0).cursor_kind() == ObjectKind::Directory);
    CK_CHECK(f.painted().find(expected.display_name()) != std::string::npos);
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(root) && f.app->location(0)->components().size() == 1);
    CK_CHECK(f.app->location(0)->components()[0] == expected && f.app->model(0).row_count() == 1);
    CK_CHECK(f.painted().find("leaf.txt") != std::string::npos);
    std::filesystem::create_directory(folder / "nested");
    CK_CHECK(f.app->navigate_parent(0));
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 2);
    CK_CHECK(f.app->navigate_parent(0));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1 && f.app->location(0)->components()[0] == expected);
    CK_CHECK(f.app->model(0).row_count() == 2);
    const auto before_editor = f.app->location(0);
    CK_CHECK(f.location_key());
    const auto exact_text = format_files_exact_location(*before_editor);
    CK_CHECK(exact_text.value() && f.editor_input()->text() == *exact_text.value());
    CK_CHECK(f.alt("p")); // Refuse a lossy ordinary-path projection.
    CK_CHECK(f.editor_input()->text() == *exact_text.value());
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before_editor && !f.app->pending(0));
    std::filesystem::remove_all(folder); // Exact extended spelling on Windows.
}

CK_TEST(files_app_home_and_end_address_file_records_and_wait_for_uncached_row_identity) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.key(ckv::Key::End));
    CK_CHECK(f.app->model(0).cursor().row == 99 && !f.app->target(0));
    CK_CHECK(!f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->model(0).cursor().row == 99 && f.app->target(0));
    CK_CHECK(f.key(ckv::Key::Home));
    CK_CHECK(f.app->model(0).cursor().row == 0 && !f.app->target(0));
    f.loaded();
    CK_CHECK(f.app->target(0) && f.app->model(0).cursor_kind() == ObjectKind::Directory);
    CK_CHECK(f.command("ck.files.enter_directory"));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().size() == 1);
}

CK_TEST(files_app_parent_replays_exact_ancestors_and_preserves_view_policy_and_other_pane) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme, {}, {}, {120, 34}, {}, true);
        f.loaded();
        const auto start = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.app->enter_directory(0));
        f.loaded();
        CK_CHECK(f.app->enter_directory(0));
        f.loaded();
        CK_CHECK(f.app->location(0)->components().size() == 2);
        auto view = f.app->view(0);
        view.show_hidden = view.ascii_case_sensitive = true;
        view.display_substring = "leaf";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        const auto before = f.app->location(0);
        CK_CHECK(f.key(ckv::Key::Backspace) && f.app->pending(0) && f.app->location(0) == before);
        f.loaded();
        CK_CHECK(f.app->location(0)->components().size() == 1 && f.app->view(0).display_substring.empty());
        CK_CHECK(f.app->view(0).show_hidden && f.app->view(0).ascii_case_sensitive);
        CK_CHECK(f.command("ck.files.parent"));
        f.loaded();
        CK_CHECK(*f.app->location(0) == *start && f.app->location(1) == other);
        CK_CHECK(f.app->model(0).row_count() == 100);
    }
}
CK_TEST(files_app_parent_failure_retains_exact_route_rows_and_range_without_affecting_other_pane) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto previous = f.app->location(0), other = f.app->location(1);
    std::filesystem::remove_all(f.scratch.path() / "left" / "left-item-0");
    CK_CHECK(f.app->navigate_parent(0));
    f.loaded();
    CK_CHECK(f.app->location(0) == previous && f.app->location(1) == other && f.app->model(0).row_count() == 1);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
}
CK_TEST(files_app_root_menu_opens_pane_local_volume_view_above_namespace_root) {
    Fixture f(ThemeScheme::HighContrast, {}, {}, {80, 24});
    f.loaded();
    const auto expected = f.app->location(0)->namespace_root();
    const auto other = f.app->location(1);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "r"}}));
    f.loaded();
    CK_CHECK(*f.app->location(0) == *expected.value() && f.app->location(1) == other);
    CK_CHECK(!f.command("ck.files.root"));
    CK_CHECK(f.command("ck.files.parent"));
    f.until([&] { return f.painted().find("Volumes and mounts above") != std::string::npos; });
    CK_CHECK(f.app->model(0).frame().columns.size() == 3);
    const auto text = f.painted();
    CK_CHECK(text.find("/fixture") != text.npos && text.find("fixturefs") != text.npos);
    CK_CHECK(text.find("3.00 MiB") != text.npos && text.find("10.0 MiB") != text.npos);
    CK_CHECK(f.key(ckv::Key::Down));
    CK_CHECK(f.app->active_pane() == 0 && f.app->model(0).cursor().row == 1);
    CK_CHECK(f.app->status(0).find("RO") != std::string::npos);
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.copy")));
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.move")));
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.rename")));
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.permanently_delete")));
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.toggle_mark")));
    CK_CHECK(f.application.commands().is_enabled(f.id("ck.files.parent")));
    CK_CHECK(f.app->status(0).find("archive capability is unknown") != std::string::npos);
    CK_CHECK(f.app->availability("ck.files.move").reason.find("volume rows are archive sources") != std::string::npos);
    CK_CHECK(f.painted().find("RO") != std::string::npos);
    const ckv::Point footer_enter{2, 23};
    CK_CHECK(f.application.dispatch(ckv::MouseEvent{
        .action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = footer_enter}));
    CK_CHECK(f.application.dispatch(ckv::MouseEvent{
        .action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = footer_enter}));
    CK_CHECK(f.painted().find("Enter folder unavailable: return to the filesystem listing") != std::string::npos);
    CK_CHECK(f.app->model(0).cursor().row == 1);
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "o"}}));
    CK_CHECK(f.key(ckv::Key::End));
    const auto move_reason = f.app->availability("ck.files.move").reason;
    CK_CHECK(f.app->status(0).find(move_reason) != std::string::npos);
    CK_CHECK(f.painted().find("Hint: Move unavailable: volume rows") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Enter));
    CK_CHECK(f.app->status(0).find(move_reason) != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    CK_CHECK(f.app->status(0).find("archive capability is unknown") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape)); // Return focus from the menu bar to the volume rows.
    CK_CHECK(f.alt("v"));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "c"}}));
    CK_CHECK(f.application.dispatch(ckv::TextEvent{"Move to other pane", false}));
    CK_CHECK(f.painted().find("Hint: Move unavailable: volume rows") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Enter));
    CK_CHECK(f.painted().find("Hint: Move unavailable: volume rows") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    CK_CHECK(f.painted().find("archive capability is unknown") != std::string::npos);
    f.terminal.resize({180, 46});
    f.loaded();
    CK_CHECK(f.key(ckv::Key::End));
    f.loaded();
    const auto last_volume = f.app->model(0).volume_cursor();
    CK_CHECK(last_volume && last_volume->identity == "fixture-mount-95");
    CK_CHECK(f.app->model(0).frame().columns.size() == 3);
    CK_CHECK(f.app->location(1) == other);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return f.painted().find("Volumes and mounts above") == std::string::npos; });
    CK_CHECK(!f.app->volume_info_pending());
    CK_CHECK(!f.app->navigate_root(0) && !f.app->navigate_parent(2));
}
CK_TEST(files_app_volume_copy_stays_disabled_when_recovery_review_is_required) {
    Fixture f(ThemeScheme::Classic, {}, {}, {80, 24}, {}, false, false, false, {}, true, true);
    f.loaded();
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
    CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "r"}}));
    f.loaded();
    CK_CHECK(f.command("ck.files.parent"));
    f.until([&] { return f.painted().find("Volumes and mounts above") != std::string::npos; });
    f.until([&] { return f.app->status(0).find("archive recovery review is required") != std::string::npos; });
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.copy")));
    CK_CHECK(f.app->availability("ck.files.copy").reason.find("archive recovery review is required") != std::string::npos);
    CK_CHECK(f.app->status(0).find("Hint: Copy unavailable: archive recovery review is required") != std::string::npos);
    CK_CHECK(f.app->availability("ck.files.inspect_archive_recovery").enabled);
    CK_CHECK(f.application.commands().is_enabled(f.id("ck.files.inspect_archive_recovery")));
    CK_CHECK(!f.app->availability("ck.files.cleanup_archive_recovery").enabled);
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.cleanup_archive_recovery")));
    CK_CHECK(f.command("ck.files.inspect_archive_recovery"));
    f.until([&] { return f.painted().find("Archive recovery review") != std::string::npos; });
    CK_CHECK(f.painted().find("Read-only inspection") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return f.painted().find("Archive recovery review") == std::string::npos; });
}
CK_TEST(files_app_archive_cleanup_requires_live_creator_ownership) {
    Fixture f(ThemeScheme::Classic, {}, {}, {80, 24}, {}, false, false, false, {}, false, false, false, true);
    f.loaded();
    f.until([&] {
        return f.app->availability("ck.files.cleanup_archive_recovery").reason.find(
            "live archive ownership is required") != std::string::npos;
    });
    CK_CHECK(!f.app->availability("ck.files.cleanup_archive_recovery").enabled);
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.cleanup_archive_recovery")));
    CK_CHECK(!f.command("ck.files.cleanup_archive_recovery"));
}

CK_TEST(files_app_operation_recovery_without_service_reports_status_reason) {
    Fixture f;
    f.loaded();
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.inspect_operation_recovery")));
    CK_CHECK(f.app->availability("ck.files.inspect_operation_recovery").reason.find(
                 "operation recovery scan is not ready") != std::string::npos);
}
CK_TEST(files_app_operation_recovery_review_is_read_only_and_modal) {
    Fixture f(ThemeScheme::Classic, {}, {}, {80, 24}, {}, false, false, false, {}, false, false, true);
    f.loaded();
    f.until([&] { return f.app->availability("ck.files.inspect_operation_recovery").enabled; });
    CK_CHECK(f.app->status(0).find("Inspect operation recovery: review is required") != std::string::npos);
    CK_CHECK(f.command("ck.files.inspect_operation_recovery"));
    f.until([&] { return f.painted().find("Operation recovery review") != std::string::npos; });
    const auto text = f.painted();
    CK_CHECK(text.find("Read-only inspection") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return f.painted().find("Operation recovery review") == std::string::npos; });
}
CK_TEST(files_app_operation_cleanup_requires_live_creator_ownership) {
    Fixture f(ThemeScheme::Classic, {}, {}, {80, 24}, {}, false, false, false, {}, false, false, false, false, true);
    f.loaded();
    f.until([&] {
        return f.app->availability("ck.files.cleanup_operation_recovery").reason.find(
            "live copy ownership is required") != std::string::npos;
    });
    CK_CHECK(!f.app->availability("ck.files.cleanup_operation_recovery").enabled);
    CK_CHECK(!f.application.commands().is_enabled(f.id("ck.files.cleanup_operation_recovery")));
    CK_CHECK(!f.command("ck.files.cleanup_operation_recovery"));
}

CK_TEST(files_app_parent_keyboard_remapping_and_modal_scope_use_suite_command_guards) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    f.application.commands().unbind_key({ckv::Key::Backspace, ckv::Modifier::None, {}});
    CK_CHECK(!f.key(ckv::Key::Backspace) && !f.app->pending(0));
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.parent"));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(!f.command("ck.files.parent") && !f.app->navigate_parent(0) && !f.app->navigate_root(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl));
    f.loaded();
    CK_CHECK(f.app->location(0)->components().empty());
}

CK_TEST(files_app_home_menu_discovers_asynchronously_preserves_policy_and_keeps_other_pane_independent) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        auto view = f.app->view(0);
        view.show_hidden = view.ascii_case_sensitive = true;
        view.display_substring = "left";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
        f.loaded();
        const auto before = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::Alt, "l"}}));
        CK_CHECK(f.application.dispatch(ckv::KeyEvent{{ckv::Key::Char, ckv::Modifier::None, "h"}}));
        CK_CHECK(f.app->pending(0) && f.app->location(0) == before && f.app->selection(0).marks->total == 1);
        f.loaded();
        CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
                 f.app->location(1) == other);
        CK_CHECK(f.app->view(0).display_substring.empty() && f.app->view(0).show_hidden &&
                 f.app->view(0).ascii_case_sensitive);
        CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.mark_range") &&
                 f.app->model(0).row_count() == 3);
    }
}
CK_TEST(files_app_blocked_home_cancels_or_is_superseded_and_never_changes_the_other_pane) {
    for (bool supersede : {false, true}) {
        Fixture f(ThemeScheme::Classic, {}, {}, {80, 24});
        f.loaded();
        const auto before = f.app->location(0), other = f.app->location(1);
        *f.block_home = true;
        CK_CHECK(f.command("ck.files.home"));
        f.home_gate->wait();
        CK_CHECK(f.app->status(0) == "Opening home location..." && f.app->location(0) == before);
        CK_CHECK(!f.command("ck.files.home") && !f.app->target(0));
        f.app->switch_pane();
        CK_CHECK(f.key(ckv::Key::Down));
        f.app->switch_pane();
        if (supersede)
            CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
        else
            CK_CHECK(f.command("ck.files.cancel_read"));
        f.home_gate->release();
        f.loaded();
        CK_CHECK(f.app->location(1) == other && f.app->model(1).cursor().row == 1);
        if (supersede)
            CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
        else
            CK_CHECK(f.app->location(0) == before && f.app->status(0).find("Read cancelled") != std::string::npos);
    }
}
CK_TEST(files_app_home_discovery_failure_retains_location_filter_marks_and_range_without_fallback) {
    Fixture f;
    f.loaded();
    auto view = f.app->view(0);
    view.display_substring = "left";
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto before = f.app->location(0);
    *f.fail_home = true;
    CK_CHECK(f.app->navigate_home(0));
    f.loaded();
    CK_CHECK(f.app->location(0) == before && f.app->view(0).display_substring == "left" &&
             f.app->model(0).row_count() == 100);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
    CK_CHECK(f.app->status(0).find("previous location retained") != std::string::npos);
}
CK_TEST(files_app_home_obeys_modal_scope_and_configurable_keymap) {
    Fixture f;
    f.loaded();
    CK_CHECK(!f.app->navigate_home(2));
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.home"));
    CK_CHECK(f.command("ck.files.view_options"));
    CK_CHECK(!f.command("ck.files.home") && !f.app->navigate_home(0));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl));
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
}
CK_TEST(files_app_location_editor_ctrl_l_changes_only_its_pane_and_preserves_view_policy_in_all_themes) {
    for (auto theme :
         {ThemeScheme::Classic, ThemeScheme::Dark, ThemeScheme::Light, ThemeScheme::Mono, ThemeScheme::HighContrast}) {
        Fixture f(theme);
        f.loaded();
        auto view = f.app->view(0);
        view.show_hidden = view.descending = true;
        view.display_substring = "left";
        CK_CHECK(f.app->change_view(0, view));
        f.loaded();
        CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
        f.loaded();
        const auto before = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.location_key() && f.app->location_editor_pending());
        CK_CHECK(!f.command("ck.files.home") && !f.command("ck.files.toggle_mark"));
        f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
        CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
        CK_CHECK(f.key(ckv::Key::Enter));
        f.until([&] { return !f.app->location_editor_pending(); });
        f.loaded();
        CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
                 f.app->location(1) == other);
        CK_CHECK(f.app->view(0).descending && f.app->view(0).show_hidden && f.app->view(0).display_substring.empty());
        CK_CHECK(f.app->selection(0).marks->total == 0 && !f.command("ck.files.mark_range"));
        CK_CHECK(f.application.focused() == f.app->grid(0));
    }
}
CK_TEST(files_app_location_editor_uses_worker_completion_and_right_accepts_the_exact_directory_proposal) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.location_key());
    auto *field = f.editor_input();
    const auto prefix = f.path_text(f.scratch.path() / "left") + "/left-item-";
    field->set_text(prefix);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!field->completion_candidate() && std::chrono::steady_clock::now() < deadline) {
        f.application.step(f.clock.now_nanos());
        f.clock.advance(1'000'000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(field->completion_candidate() && field->has_completion_preview());
    if (field->completion_candidate()) {
        CK_CHECK(field->completion_candidate()->text == prefix + "0/");
        CK_CHECK(f.key(ckv::Key::Right));
        CK_CHECK(field->text() == prefix + "0/");
    }
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0)->components().empty());
}
namespace {
// Observe the actual existing form label, not a private provider status getter.
bool has_location_feedback(ckv::ui::Application &application, std::string_view text) {
    std::vector<ckv::widgets::Label *> labels;
    find_views(application.root(), labels);
    for (const auto *label : labels)
        if (label->text() == text)
            return true;
    return false;
}
} // namespace

CK_TEST(files_app_location_completion_native_failure_edit_success_preserves_route_and_right_acceptance) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    const auto original = f.app->location(0), other = f.app->location(1);
    CK_CHECK(f.location_key());
    auto *field = f.editor_input();
    const auto missing = f.path_text(f.scratch.path() / "missing-completion-parent") + "/item-";
    field->set_text(missing); // Real native directory-open failure on the worker.
    f.until([&] { return has_location_feedback(f.application, "Folder completion unavailable; edit to retry."); });
    CK_CHECK(field->text() == missing && !field->completion_candidate());
    CK_CHECK(f.app->location(0) == original && f.app->location(1) == other);

    const auto prefix = f.path_text(f.scratch.path() / "left") + "/left-item-";
    field->set_text(prefix);
    CK_CHECK(has_location_feedback(f.application, "Path: a new absolute location"));
    CK_CHECK(field->text() == prefix && f.app->location(0) == original);
    f.until([&] { return field->has_completion_preview(); });
    CK_CHECK(has_location_feedback(f.application, "Path: a new absolute location"));
    CK_CHECK(field->completion_candidate() && field->completion_candidate()->text == prefix + "0/");
    CK_CHECK(f.key(ckv::Key::Right) && field->text() == prefix + "0/");
    CK_CHECK(f.app->location(0) == original && f.app->location(1) == other);
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0) != original && f.app->model(0).row_count() == 2);
    CK_CHECK(f.app->location(1) == other);
}

CK_TEST(files_location_controller_rejects_replayed_worker_failure_by_ticket_and_revision) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    const auto original = f.app->location(0), other = f.app->location(1);
    // Use the public controller delivery boundary with actual native worker
    // replies. Delayed delivery is deterministic; no fabricated private ticket.
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34});
    ckv::ui::Application application(terminal, clock);
    SuiteShell shell(application, {.application_name = "Files"});
    FilesCompletionService service(native_files_pane_backend(address(f.scratch.path() / "completion-cache-left")));
    auto draft = FilesLocationDraft::start(*original);
    if (!draft.value())
        throw std::runtime_error("completion dialog draft unavailable");
    FilesLocationDialog dialog(application, shell.desktop(), shell.roles(), std::move(draft).take_value(),
                               &service, [](std::optional<FilesPaneLocation>) {});
    auto *field = dialog.input();
    if (!field)
        throw std::runtime_error("completion dialog input unavailable");
    const auto next_reply = [&]() {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            if (auto reply = service.take())
                return std::move(*reply);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < end);
        throw std::runtime_error("completion worker reply timed out");
    };
    // Finish the seed request before changing the query so it cannot race the
    // specific native failure whose real ticket/revision is retained below.
    dialog.publish_completion(next_reply());
    const auto missing = f.path_text(f.scratch.path() / "missing-replay-parent") + "/item-";
    field->set_text(missing);
    const auto failed = next_reply();
    CK_CHECK(failed.state == FilesCompletionReplyState::Failed && failed.error);
    dialog.publish_completion(failed);
    CK_CHECK(has_location_feedback(application, "Folder completion unavailable; edit to retry."));
    const auto prefix = f.path_text(f.scratch.path() / "left") + "/left-item-";
    field->set_text(prefix);
    CK_CHECK(has_location_feedback(application, "Path: a new absolute location"));
    dialog.publish_completion(failed); // Old reply arriving during the new request.
    CK_CHECK(has_location_feedback(application, "Path: a new absolute location"));
    const auto ready = next_reply();
    CK_CHECK(ready.state == FilesCompletionReplyState::Ready && ready.page);
    CK_CHECK(ready.request != failed.request && ready.revision != failed.revision);
    // Corrupt one envelope field at a time, using only genuinely issued
    // tickets/revisions, to prove both controller guards independently.
    auto wrong_ticket = failed;
    wrong_ticket.revision = ready.revision;
    dialog.publish_completion(std::move(wrong_ticket));
    auto wrong_revision = failed;
    wrong_revision.request = ready.request;
    dialog.publish_completion(std::move(wrong_revision));
    CK_CHECK(has_location_feedback(application, "Path: a new absolute location"));
    CK_CHECK(field->text() == prefix && !field->has_completion_preview());
    dialog.publish_completion(ready);
    CK_CHECK(field->completion_candidate() && field->completion_candidate()->text == prefix + "0/");
    CK_CHECK(application.dispatch(ckv::KeyEvent{{ckv::Key::Right, ckv::Modifier::None, {}}}));
    CK_CHECK(field->text() == prefix + "0/");
    dialog.publish_completion(failed); // Cannot replace a later successful result.
    CK_CHECK(has_location_feedback(application, "Path: a new absolute location"));
    CK_CHECK(f.app->location(0) == original && f.app->location(1) == other);
    dialog.close();
    application.step(clock.now_nanos());
    dialog.publish_completion(failed); // Detached controller must remain inert.
    service.request_stop();
    service.join();
}

CK_TEST(files_app_location_completion_uses_fixed_hints_for_malformed_and_oversized_worker_errors) {
    for (const bool oversized : {false, true}) {
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        std::string diagnostic = "private-native-path/";
        diagnostic += '\x1b';
        diagnostic += "[31m";
        diagnostic += '\0';
        diagnostic += std::string(oversized ? 128 * 1024 : 1024, 'x');
        Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, false, false, false, {},
                  false, false, false, false, false,
                  [calls, diagnostic]() -> Observation<std::shared_ptr<IndexFileFactory>> {
                      calls->fetch_add(1);
                      return Observation<std::shared_ptr<IndexFileFactory>>::unavailable(
                          ObservationState::Denied, {ErrorDomain::Adapter, 0x7fffffff, diagnostic});
                  });
        f.loaded();
        const auto original = f.app->location(0), other = f.app->location(1);
        CK_CHECK(f.location_key());
        auto *field = f.editor_input();
        // Opening the form selects its seed: completion deliberately does not
        // query selected text. Record that state, then make a distinct edit so
        // this test exercises an actual current worker request.
        std::fprintf(stderr, "COMPLETION_ERROR_FIXTURE oversized=%d initial_selected=%d initial_calls=%u "
                             "initial_default_hint=%d initial_denied_hint=%d initial_limit_hint=%d\n",
                     static_cast<int>(oversized), static_cast<int>(field->has_selection()), calls->load(),
                     static_cast<int>(has_location_feedback(f.application, "Path: a new absolute location")),
                     static_cast<int>(has_location_feedback(f.application, "Folder completion denied; edit to retry.")),
                     static_cast<int>(has_location_feedback(f.application, "Completion limit reached; narrow the location.")));
        const auto typed = f.path_text(f.scratch.path() / "left") + "/left-item-";
        field->set_text(typed);
        CK_CHECK(!field->has_selection() && field->text() == typed);
        const std::string_view expected = oversized ? "Completion limit reached; narrow the location."
                                                   : "Folder completion denied; edit to retry.";
        f.until([&] { return has_location_feedback(f.application, expected); });
        std::fprintf(stderr, "COMPLETION_ERROR_FIXTURE oversized=%d current_selected=%d calls=%u hint=%.*s\n",
                     static_cast<int>(oversized), static_cast<int>(field->has_selection()), calls->load(),
                     static_cast<int>(expected.size()), expected.data());
        CK_CHECK(calls->load() != 0 && field->text() == typed && !field->completion_candidate());
        CK_CHECK(f.app->location(0) == original && f.app->location(1) == other);
        std::vector<ckv::widgets::Label *> labels;
        find_views(f.application.root(), labels);
        for (const auto *label : labels) {
            CK_CHECK(label->text().find("private-native-path") == std::string::npos);
            CK_CHECK(label->text().find('\x1b') == std::string::npos);
            CK_CHECK(label->text().find('\0') == std::string::npos);
        }
        CK_CHECK(f.key(ckv::Key::Escape));
        f.until([&] { return !f.app->location_editor_pending(); });
    }
}

CK_TEST(files_app_location_editor_unchanged_accept_and_format_switches_preserve_exact_route_marks_and_range) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, true);
    f.loaded();
    CK_CHECK(f.app->enter_directory(0));
    f.loaded();
    CK_CHECK(f.command("ck.files.range_start") && f.command("ck.files.toggle_mark"));
    f.loaded();
    const auto before = f.app->location(0);
    const auto target = *f.app->target(0);
    CK_CHECK(before->components().size() == 1 && f.location_key());
    CK_CHECK(f.alt("e"));
    const auto exact = format_files_exact_location(*before);
    CK_CHECK(exact.value() && f.editor_input()->text() == *exact.value());
    CK_CHECK(f.alt("p"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
    CK_CHECK(f.app->target(0)->navigation == target.navigation && f.app->target(0)->row.row == target.row.row);
    CK_CHECK(f.app->selection(0).marks->total == 1 && f.app->status(0).find("Range start set") != std::string::npos);
}
CK_TEST(files_app_location_editor_vetoes_invalid_text_and_actual_service_limits_without_native_navigation) {
    FilesPaneServiceLimits limits;
    limits.location_components = 1;
    Fixture f(ThemeScheme::Classic, {}, limits);
    f.loaded();
    const auto before = f.app->location(0);
    CK_CHECK(f.location_key());
    f.editor_input()->set_text("relative");
    CK_CHECK(f.key(ckv::Key::Enter) && f.app->location_editor_pending() && !f.editor_input()->valid());
    CK_CHECK(f.painted().find("Enter a valid absolute") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
    CK_CHECK(f.location_key() && f.alt("e"));
    auto route = *before;
    for (auto name : {"first", "second"}) {
#ifdef _WIN32
        const auto native =
            *NativeName::from_windows(std::u16string(name, name + std::char_traits<char>::length(name)));
#else
        const auto native = *NativeName::from_posix(name);
#endif
        const auto next = route.descend(native, {}, 4);
        if (!next.value())
            throw std::runtime_error("limit fixture route");
        route = *next.value();
    }
    const auto exact = format_files_exact_location(route);
    CK_CHECK(exact.value());
    f.editor_input()->set_text(*exact.value());
    CK_CHECK(f.key(ckv::Key::Enter) && f.app->location_editor_pending());
    CK_CHECK(f.painted().find("exceeds a configured") != std::string::npos);
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
}
CK_TEST(files_app_location_editor_recovers_an_initial_native_failure_without_a_completed_location) {
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, false, true);
    f.loaded();
    CK_CHECK(!f.app->location(0) && !f.app->model(0).current());
    CK_CHECK(f.location_key());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") &&
             f.app->model(0).row_count() == 3);
}
CK_TEST(files_app_location_editor_empty_recovery_never_guesses_a_target_after_initial_submission_rejection) {
    FilesPaneServiceLimits limits;
    limits.path_bytes = 1024;
    Fixture f(ThemeScheme::Classic, {}, limits, {120, 34}, {}, false, false, true);
    f.loaded();
    const auto other = f.app->location(1);
    CK_CHECK(!f.app->location(0) && f.location_key() && f.editor_input()->text().empty());
    CK_CHECK(f.key(ckv::Key::Enter) && f.app->location_editor_pending());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right") && f.app->model(0).current());
    CK_CHECK(f.app->location(1) == other);
}
CK_TEST(files_app_location_editor_does_not_retarget_a_pane_superseded_while_the_form_is_open) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.location_key());
    CK_CHECK(f.app->navigate(0, address(f.scratch.path() / "right")));
    f.loaded();
    const auto replacement = f.app->location(0);
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "left"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->location(0) == replacement && !f.app->pending(0));
    CK_CHECK(f.app->status(0).find("Location changed while its editor was open") != std::string::npos);
}
CK_TEST(files_app_location_editor_quit_retirement_and_external_detach_never_submit_unaccepted_text) {
    for (int action : {0, 1, 2, 3}) {
        Fixture f;
        f.loaded();
        const auto before = f.app->location(0);
        CK_CHECK(f.location_key());
        f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
        if (action == 0) {
            f.app->request_close();
            CK_CHECK(f.exits == 0);
            f.until([&] { return f.exits == 1; });
            CK_CHECK(f.app->location(0) == before);
        } else if (action == 1) {
            f.app.reset();
            f.application.step(f.clock.now_nanos());
            CK_CHECK(f.exits == 0);
        } else {
            ckv::widgets::Window *dialog = nullptr;
            for (auto *view = f.application.focused(); view; view = view->parent())
                if (auto *candidate = dynamic_cast<ckv::widgets::Window *>(view)) {
                    dialog = candidate;
                    break;
                }
            if (!dialog || !dialog->parent())
                throw std::runtime_error("missing attached location window");
            if (action == 3)
                CK_CHECK(f.key(ckv::Key::Enter));
            auto detached = dialog->parent()->remove_child(dialog);
            (void)detached;
            if (action == 3) {
                f.app.reset();
                f.application.step(f.clock.now_nanos());
                CK_CHECK(f.exits == 0);
            } else {
                f.until([&] { return !f.app->location_editor_pending(); });
                CK_CHECK(f.app->location(0) == before && !f.app->pending(0));
            }
        }
    }
}
CK_TEST(files_app_location_editor_scrolls_after_narrow_resize_and_accepts_through_the_real_go_button) {
    Fixture f;
    f.loaded();
    CK_CHECK(f.location_key());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    f.terminal.resize({40, 12});
    f.application.step(f.clock.now_nanos());
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), buttons);
    ckv::widgets::Button *go = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Go")
            go = button;
    if (!go)
        throw std::runtime_error("missing real Go button");
    const auto rect = go->absolute_bounds();
    CK_CHECK(rect.width > 0 && rect.height > 0);
    const ckv::Point at{rect.x + rect.width / 2, rect.y};
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Down, .button = ckv::MouseButton::Left, .cell = at}));
    CK_CHECK(f.application.dispatch(
        ckv::MouseEvent{.action = ckv::MouseAction::Up, .button = ckv::MouseButton::Left, .cell = at}));
    f.until([&] { return !f.app->location_editor_pending(); });
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
}
CK_TEST(files_app_location_command_obeys_runtime_remapping_and_unrelated_focus_scope) {
    Fixture f;
    f.loaded();
    auto other = std::make_unique<ckv::widgets::InputLine>();
    other->set_bounds({0, 0, 20, 1});
    auto *outside = f.application.root().add_child(std::move(other));
    f.application.set_focus(outside);
    CK_CHECK(!f.command("ck.files.location"));
    f.application.root().remove_child(outside);
    f.application.set_focus(f.app->grid(0));
    f.application.commands().unbind_key({ckv::Key::Char, ckv::Modifier::Ctrl, "l"});
    CK_CHECK(!f.location_key() && !f.app->location_editor_pending());
    f.application.commands().bind_key({ckv::Key::F8, ckv::Modifier::Ctrl, {}}, f.id("ck.files.location"));
    CK_CHECK(f.key(ckv::Key::F8, ckv::Modifier::Ctrl) && f.app->location_editor_pending());
    CK_CHECK(f.key(ckv::Key::Escape));
    f.until([&] { return !f.app->location_editor_pending(); });
}
CK_TEST(files_app_location_editor_can_supersede_a_blocked_home_request_without_waiting_in_dispatch) {
    Fixture f;
    f.loaded();
    const auto before = f.app->location(0);
    *f.block_home = true;
    CK_CHECK(f.app->navigate_home(0));
    f.home_gate->wait();
    CK_CHECK(f.app->pending(0) && f.location_key() && f.app->location_editor_pending());
    f.editor_input()->set_text(f.path_text(f.scratch.path() / "right"));
    CK_CHECK(f.key(ckv::Key::Enter));
    f.until([&] { return !f.app->location_editor_pending(); });
    CK_CHECK(f.app->pending(0) && f.app->location(0) == before);
    f.home_gate->release();
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == address(f.scratch.path() / "right"));
}
CK_TEST(files_app_history_and_bookmarks_change_only_after_worker_acknowledgment) {
    Fixture f;
    f.loaded();
    const auto left = f.app->location(0);
    const auto right = address(f.scratch.path() / "right");
    CK_CHECK(f.app->history_size(0) == 1 && f.app->history_position(0) == 0);
    CK_CHECK(f.app->navigate(0, right));
    CK_CHECK(f.app->history_size(0) == 1);
    f.loaded();
    CK_CHECK(f.app->history_size(0) == 2 && f.app->history_position(0) == 1 &&
             f.app->location(0)->root_address() == right);
    CK_CHECK(f.app->add_bookmark(0) && f.app->bookmark_count(0) == 1);
    CK_CHECK(!f.app->add_bookmark(0) && f.app->bookmark_count(0) == 1);
    CK_CHECK(f.app->navigate_history_back(0));
    CK_CHECK(f.app->pending(0) && f.app->location(0) && *f.app->location(0) != *left);
    f.loaded();
    CK_CHECK(f.app->history_position(0) == 0 && f.app->location(0) && *f.app->location(0) == *left);
    CK_CHECK(f.app->navigate_history_forward(0));
    f.loaded();
    CK_CHECK(f.app->history_position(0) == 1 && f.app->location(0)->root_address() == right);
    CK_CHECK(f.app->navigate_bookmark(0, 0));
    f.loaded();
    CK_CHECK(f.app->location(0)->root_address() == right);
    CK_CHECK(f.app->remove_bookmark(0) && f.app->bookmark_count(0) == 0);
}
CK_TEST(files_app_persists_acknowledged_view_and_bookmark_changes) {
    ckv::MemoryFileSystem files;
    FilesSettingsStore store(files, "/user/ck-files.settings");
    auto loaded = store.load();
    FilesSettingsService settings_worker(store);
    FilesSettingsBinding binding{
        std::move(loaded.value), std::move(loaded.diagnostic), &settings_worker};
    Fixture f(ThemeScheme::Classic, {}, {}, {120, 34}, {}, false, false, false, std::move(binding));
    f.loaded();
    CK_CHECK(f.app->add_bookmark(0));
    f.until([&] { return settings_worker.progress().saved_revision == 1; });
    auto saved = parse_files_settings(files.read_file("/user/ck-files.settings")->contents);
    CK_CHECK(saved && saved->bookmarks[0].size() == 1 && saved->bookmarks[0][0] == *f.app->location(0));
    auto view = f.app->view(0);
    view.show_hidden = true;
    CK_CHECK(f.app->change_view(0, view));
    f.loaded();
    f.until([&] { return settings_worker.progress().saved_revision == 2; });
    saved = parse_files_settings(files.read_file("/user/ck-files.settings")->contents);
    CK_CHECK(saved && saved->views[0].show_hidden);
}
CK_TEST(files_app_operation_review_is_modal_and_cancel_is_plan_bound) {
    Fixture f;
    f.loaded();
    std::optional<ConsentReceipt> receipt;
    CK_CHECK(f.app->review_operation(review_model(), NameEncoding::PosixBytes,
        [&](const ConsentReceipt &value) { return Observation<ConsentReceipt>::available(value); },
        [&](Observation<ConsentReceipt> result) {
            if (result.value())
                receipt = std::move(result).take_value();
        }));
    CK_CHECK(f.app->operation_review_pending());
    CK_CHECK(!f.command("ck.files.toggle_mark"));
    CK_CHECK(f.key(ckv::Key::Escape));
    f.application.step(f.clock.now_nanos());
    f.application.step(f.clock.now_nanos());
    CK_CHECK(!f.app->operation_review_pending() && receipt && receipt->action == ConsentAction::Cancel &&
        receipt->plan.is_valid());
}
CK_TEST(files_app_operation_preparation_rejects_invalid_worker_evidence_before_opening_review) {
    Fixture f;
    f.loaded();
    bool delivered = false;
    CK_CHECK(!f.app->prepare_operation(FilesOperationPlanningRequest{}, NameEncoding::PosixBytes,
        [&](const ConsentReceipt &value) { return Observation<ConsentReceipt>::available(value); },
        [&](Observation<ConsentReceipt>) { delivered = true; }));
    CK_CHECK(!f.app->prepare_operation(FilesOperationPanePlanningRequest{}, NameEncoding::PosixBytes,
        [&](const ConsentReceipt &value) { return Observation<ConsentReceipt>::available(value); },
        [&](Observation<ConsentReceipt>) { delivered = true; }));
    CK_CHECK(!f.app->operation_review_pending() && !delivered);
}
CK_TEST(files_app_operation_review_revalidates_accepted_decisions_before_delivery) {
    Fixture f;
    f.loaded();
    bool revalidated = false;
    bool delivered_failure = false;
    CK_CHECK(f.app->review_operation(review_model(), NameEncoding::PosixBytes,
        [&](const ConsentReceipt &) {
            revalidated = true;
            return Observation<ConsentReceipt>::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::PlanDecisionStale),
                    "files_app.test.stale"});
        },
        [&](Observation<ConsentReceipt> result) {
            delivered_failure = !result.value() && result.unavailable_detail() &&
                                result.unavailable_detail()->state == ObservationState::Changed &&
                                result.unavailable_detail()->error.code ==
                                    static_cast<std::int64_t>(AdapterError::PlanDecisionStale);
        }));
    f.application.step(f.clock.now_nanos());
    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(f.application.root(), combos);
    find_views(f.application.root(), inputs);
    find_views(f.application.root(), buttons);
    CK_CHECK(combos.size() == 1 && !inputs.empty());
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply decision")
            apply = button;
    CK_CHECK(apply != nullptr);
    combos.front()->set_selected_index(3); // Keep both.
    ckv::widgets::InputLine *name = nullptr;
    for (auto *input : inputs) {
        bool inside_combo = false;
        for (auto *parent = input->parent(); parent; parent = parent->parent())
            inside_combo = inside_combo || dynamic_cast<ckv::widgets::ComboBox *>(parent) != nullptr;
        if (!inside_combo)
            name = input;
    }
    CK_CHECK(name != nullptr);
    name->set_text("copy");
    CK_CHECK(apply->activate_mnemonic("a"));
    for (int i = 0; i < 5; ++i)
        f.application.step(f.clock.now_nanos());
    CK_CHECK(revalidated);
    CK_CHECK(delivered_failure);
    CK_CHECK(!f.app->operation_review_pending());
}
