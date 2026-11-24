// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WP-07 ck-files 100,000-entry directory, indexed-pane, rendering, and
// recovery-overlap scale benchmark. Wall-clock timings are diagnostic outside
// the reference host; deterministic entry counts, index byte budgets, cell
// budgets, and zero-idle-output assertions in `--gate` are hard failures on
// every machine.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "ckbench.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/widgets/cell_grid.hpp"

#include "directory_index_listing_job.hpp"
#include "files_operation_recovery.hpp"
#include "files_pane_grid.hpp"
#include "indexed_pane.hpp"

namespace {

using namespace ck::files;
using namespace ck::vision;

struct BenchManualClock final : ckv::Clock {
    std::int64_t now = 0;
    std::int64_t now_nanos() const noexcept override { return now; }
};

std::uint64_t peak_rss_kib() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize / 1024U);
    return 0;
#elif defined(__APPLE__)
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0)
        return static_cast<std::uint64_t>(usage.ru_maxrss / 1024U);
    return 0;
#else
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0)
        return static_cast<std::uint64_t>(usage.ru_maxrss);
    return 0;
#endif
}

struct BackingStats {
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
    std::uint64_t files_created = 0;
};

struct MemorySlot {
    std::vector<std::byte> bytes;
    std::shared_ptr<BackingStats> stats;
};

class MemoryIndexByteFile final : public IndexByteFile {
  public:
    explicit MemoryIndexByteFile(std::shared_ptr<MemorySlot> slot) : slot_(std::move(slot)) {}
    ~MemoryIndexByteFile() override {
        if (slot_ && slot_->stats) {
            slot_->stats->current_bytes -= static_cast<std::uint64_t>(slot_->bytes.size());
        }
        if (slot_) {
            slot_->bytes.clear();
            slot_->bytes.shrink_to_fit();
        }
    }
    Observation<std::uint64_t> size() override {
        return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(slot_->bytes.size()));
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> data) override {
        const auto at = static_cast<std::size_t>(offset);
        const auto old_size = slot_->bytes.size();
        if (at + data.size() > old_size) {
            slot_->bytes.resize(at + data.size());
            slot_->stats->current_bytes += static_cast<std::uint64_t>(slot_->bytes.size() - old_size);
            slot_->stats->peak_bytes = std::max(slot_->stats->peak_bytes, slot_->stats->current_bytes);
        }
        std::copy(data.begin(), data.end(), slot_->bytes.begin() + static_cast<std::ptrdiff_t>(at));
        return Observation<std::size_t>::available(data.size());
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> data) override {
        const auto at = static_cast<std::size_t>(offset);
        const auto available = at < slot_->bytes.size() ? std::min(data.size(), slot_->bytes.size() - at) : 0U;
        if (available > 0)
            std::copy_n(slot_->bytes.begin() + static_cast<std::ptrdiff_t>(at), available, data.begin());
        return Observation<std::size_t>::available(available);
    }

  private:
    std::shared_ptr<MemorySlot> slot_;
};

class MemoryIndexFactory final : public IndexFileFactory {
  public:
    explicit MemoryIndexFactory(std::shared_ptr<BackingStats> stats) : stats_(std::move(stats)) {}
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        ++stats_->files_created;
        auto slot = std::make_shared<MemorySlot>();
        slot->stats = stats_;
        return Observation<std::unique_ptr<IndexByteFile>>::available(
            std::make_unique<MemoryIndexByteFile>(std::move(slot)));
    }

  private:
    std::shared_ptr<BackingStats> stats_;
};

ObjectIdentity synthetic_identity(const AdapterScope &scope, std::uint64_t object_id) {
    return {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
            NativeIdentifier::from_u64(object_id), {}};
}

NativeName synthetic_name(std::uint64_t index) {
    char buffer[32]{};
    const bool is_dir = (index % 20U) == 0U;
    std::snprintf(buffer, sizeof(buffer), is_dir ? "dir-%06llu" : "file-%06llu.dat",
                  static_cast<unsigned long long>(index));
    return *NativeName::from_posix(buffer);
}

class SyntheticOpenedEntry final : public OpenedEntry {
  public:
    SyntheticOpenedEntry(AdapterScope scope, std::uint64_t index) : scope_(std::move(scope)), index_(index) {}
    Observation<BasicMetadata> inspect_basic() const override {
        BasicMetadata meta;
        const bool is_dir = (index_ % 20U) == 0U;
        meta.kind = Observation<ObjectKind>::available(is_dir ? ObjectKind::Directory : ObjectKind::RegularFile);
        meta.identity = Observation<ObjectIdentity>::available(synthetic_identity(scope_, index_ + 2U));
        if (!is_dir)
            meta.logical_size = Observation<std::uint64_t>::available(((index_ * 2654435761ULL) % 10'000'000ULL) + 1U);
        meta.modified = Observation<FileTimestamp>::available(
            *FileTimestamp::from_unix(1'700'000'000LL + static_cast<std::int64_t>(index_ % 86'400U), 0));
        return Observation<BasicMetadata>::available(std::move(meta));
    }

  private:
    AdapterScope scope_;
    std::uint64_t index_ = 0;
};

class LargeSyntheticDirectoryCursor final : public DirectoryCursor {
  public:
    LargeSyntheticDirectoryCursor(AdapterScope scope, std::uint64_t count)
        : DirectoryCursor(Observation<ObjectIdentity>::available(synthetic_identity(scope, 1))),
          scope_(std::move(scope)), count_(count) {}

    Observation<AdapterScope> source_scope() const override {
        return Observation<AdapterScope>::available(scope_);
    }

    Observation<BasicMetadata> inspect_basic() const override {
        BasicMetadata meta;
        meta.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        meta.identity = Observation<ObjectIdentity>::available(synthetic_identity(scope_, 1));
        meta.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(1'700'000'000LL, 0));
        return Observation<BasicMetadata>::available(std::move(meta));
    }

    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName &name, EntryAccess) const override {
        const auto *bytes = name.posix_bytes();
        std::uint64_t index = 0;
        if (bytes) {
            const auto dash = bytes->find('-');
            if (dash != std::string::npos) {
                for (std::size_t i = dash + 1; i < bytes->size() && (*bytes)[i] >= '0' && (*bytes)[i] <= '9'; ++i)
                    index = index * 10U + static_cast<std::uint64_t>((*bytes)[i] - '0');
            }
        }
        return Observation<std::unique_ptr<OpenedEntry>>::available(
            std::make_unique<SyntheticOpenedEntry>(scope_, index));
    }

  private:
    Observation<DirectoryStep> next_step() override {
        if (next_ >= count_)
            return Observation<DirectoryStep>::available(DirectoryEnd{});
        // Emit in scrambled order so external name-sorting does genuine work.
        const std::uint64_t scrambled = (next_ * 7919U + 104729U) % count_;
        ++next_;
        return Observation<DirectoryStep>::available(synthetic_name(scrambled));
    }

    AdapterScope scope_;
    std::uint64_t count_ = 0;
    std::uint64_t next_ = 0;
};

FilesOperationRecoveryScan thousand_journal_scan(const ObjectIdentity &target_source,
                                                 const ObjectIdentity &target_dest) {
    FilesOperationRecoveryScan scan;
    scan.journals.reserve(1000);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        FilesOperationRecoveryItem item;
        item.display_name = ".ck-copy-journal-" + std::to_string(i);
        item.kind = FilesOperationJournalKind::Transfer;

        FilesOperationRecoveryReport report;
        report.journal_id.bytes[0] = static_cast<std::byte>(i & 0xffU);
        report.journal_id.bytes[1] = static_cast<std::byte>((i >> 8U) & 0xffU);

        OperationRecoveryItem rec_item;
        rec_item.item = 0;
        rec_item.state = (i == 999) ? OperationJournalState::Copied : OperationJournalState::Complete;
        rec_item.action = (i == 999) ? OperationRecoveryAction::InspectUnpublishedStaging
                                     : OperationRecoveryAction::IgnoreTerminal;
        rec_item.requires_reconciliation = (i == 999);
        report.inspection.report.items.push_back(rec_item);
        report.inspection.report.requires_reconciliation = (i == 999);

        NativeOperationRecoveryArtifact artifact;
        artifact.item = 0;
        artifact.evidence = NativeOperationRecoveryEvidence{
            *NativeName::from_posix(".ck-staging-" + std::to_string(i)),
            *NativeName::from_posix("item-" + std::to_string(i)),
            *NativeDurableIdentity::from_live(
                ObjectIdentity{target_source.scope, IdentityScheme::PosixDeviceInode,
                               NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(900'000U + i), {}}),
            *NativeDurableIdentity::from_live(
                (i == 999)
                    ? target_source
                    : ObjectIdentity{target_source.scope, IdentityScheme::PosixDeviceInode,
                                     NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(500'000U + i), {}}),
            *NativeDurableIdentity::from_live(
                (i == 999)
                    ? target_dest
                    : ObjectIdentity{target_dest.scope, IdentityScheme::PosixDeviceInode,
                                     NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(700'000U + i), {}}),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            StagingNamespace::Named,
            *NativeName::from_posix("item-" + std::to_string(i)),
        };
        report.inspection.artifacts.push_back(std::move(artifact));
        item.report = std::move(report);
        scan.journals.push_back(std::move(item));
    }
    scan.requires_reconciliation = true;
    return scan;
}

} // namespace

// Usage: files_bench [--gate], as todo_bench.
int main(int argc, char **argv) {
    const std::optional<ckbench::Mode> mode = ckbench::mode_from_arguments(argc, argv);
    if (!mode) {
        std::fprintf(stderr, "usage: files_bench [--gate]\n");
        return 2;
    }
    const ckbench::Runner bench(*mode);

    constexpr std::uint64_t kEntryCount = 100'000;
    constexpr std::uint64_t kIndexBytesBudget = 128ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kViewportRows = 40;
    constexpr std::size_t kFrameCellsBudget = 140U * 40U;

    const AdapterScope scope;
    auto stats = std::make_shared<BackingStats>();
    auto factory = std::make_shared<MemoryIndexFactory>(stats);
    auto budget = std::make_shared<IndexStorageBudget>(256ULL * 1024ULL * 1024ULL, 64);

    IndexedPane pane;
    PaneView initial_view;
    initial_view.sort = PaneSortKey::Name;
    initial_view.descending = false;
    initial_view.folders_first = true;

    DirectoryIndexListingJob::Result completed_listing;
    bool budgets_hold = true;
    std::size_t sink = 0;

    // 1. Full 100,000-entry directory scan + external name sort + duplicate check.
    bench.run("files_index_scan_100k", 1, [&] {
        const auto request = pane.begin_listing(initial_view);
        auto cursor = std::make_unique<LargeSyntheticDirectoryCursor>(scope, kEntryCount);
        auto listing = std::make_unique<DirectoryListing>(std::move(cursor), DirectoryListingLimits{});
        DirectoryIndexListingJob job(request, std::move(listing), StorageModel::LinuxStatx, factory, budget,
                                     std::array<std::byte, 16>{std::byte{0x42}});
        DirectoryIndexListingState state = job.state();
        while (state == DirectoryIndexListingState::Collecting || state == DirectoryIndexListingState::SortingNames ||
               state == DirectoryIndexListingState::CheckingNames) {
            state = job.advance();
        }
        const auto result = job.completed();
        if (state != DirectoryIndexListingState::Complete || !result.value() ||
            (*result.value())->store()->rows() != kEntryCount) {
            std::fprintf(stderr, "files_bench: scan failed (state=%d)\n", static_cast<int>(state));
            budgets_hold = false;
            return;
        }
        completed_listing = *result.value();
        sink += static_cast<std::size_t>(completed_listing->store()->rows());
    });

    const std::uint64_t index_bytes_after_scan = stats->current_bytes;
    if (!completed_listing || index_bytes_after_scan > kIndexBytesBudget) {
        std::fprintf(stderr, "files_bench: index bytes budget failed (%llu > %llu)\n",
                     static_cast<unsigned long long>(index_bytes_after_scan),
                     static_cast<unsigned long long>(kIndexBytesBudget));
        budgets_hold = false;
    }

    // 2. Build initial ordered pane data and publish into IndexedPane, then reorder by Size descending.
    IndexedPaneDataJob::Result initial_data;
    if (completed_listing) {
        IndexedPaneDataJob job(completed_listing, initial_view);
        IndexedPaneDataJobState state = job.state();
        while (state == IndexedPaneDataJobState::Sorting || state == IndexedPaneDataJobState::Indexing ||
               state == IndexedPaneDataJobState::CountingMarks) {
            state = job.advance();
        }
        const auto result = job.completed();
        if (state != IndexedPaneDataJobState::Complete || !result.value() ||
            (*result.value())->visible_rows() != kEntryCount ||
            pane.publish(*result.value()) != IndexedPanePublish::Applied) {
            std::fprintf(stderr, "files_bench: initial publish failed (state=%d)\n", static_cast<int>(state));
            budgets_hold = false;
        } else {
            initial_data = *result.value();
        }
    }

    IndexedPaneDataJob::Result sorted_data;
    PaneView sorted_view;
    sorted_view.sort = PaneSortKey::Size;
    sorted_view.descending = true;
    sorted_view.folders_first = true;
    bench.run("files_index_sort_100k", 1, [&] {
        if (!initial_data) {
            budgets_hold = false;
            return;
        }
        const auto sort_req = pane.begin_view_change(pane.state_stamp(), sorted_view);
        if (!sort_req.value()) {
            std::fprintf(stderr, "files_bench: begin_view_change(sort) failed\n");
            budgets_hold = false;
            return;
        }
        IndexedPaneDataJob job(initial_data, *sort_req.value(), sorted_view);
        IndexedPaneDataJobState state = job.state();
        while (state == IndexedPaneDataJobState::Sorting || state == IndexedPaneDataJobState::Indexing ||
               state == IndexedPaneDataJobState::CountingMarks) {
            state = job.advance();
        }
        const auto result = job.completed();
        if (state != IndexedPaneDataJobState::Complete || !result.value() ||
            (*result.value())->visible_rows() != kEntryCount) {
            std::fprintf(stderr, "files_bench: sort failed (state=%d)\n", static_cast<int>(state));
            budgets_hold = false;
            return;
        }
        sorted_data = *result.value();
        sink += static_cast<std::size_t>(sorted_data->visible_rows());
    });

    if (!sorted_data || pane.publish(sorted_data) != IndexedPanePublish::Applied) {
        std::fprintf(stderr, "files_bench: publish(sorted_data) failed\n");
        budgets_hold = false;
    }

    const auto sorted_stamp = pane.state_stamp();

    // 3. Slice bounded 40-row pages from top, middle, and tail of 100,000 rows.
    std::shared_ptr<const IndexedPanePage> sample_page;
    IndexedPanePageLimits page_limits;
    page_limits.display.rows = kViewportRows;
    const std::array<std::uint64_t, 3> offsets{0U, kEntryCount / 2U, kEntryCount - kViewportRows};
    bench.run("files_page_slice_100k", 100, [&] {
        for (const auto first : offsets) {
            const auto capture = pane.capture_page(sorted_stamp, first, page_limits);
            if (capture.state != IndexedPaneCaptureState::Ready || !capture.page ||
                capture.page->rows().size() != kViewportRows || capture.page->total_rows() != kEntryCount) {
                std::fprintf(stderr, "files_bench: capture_page(%llu) failed (state=%d)\n",
                             static_cast<unsigned long long>(first), static_cast<int>(capture.state));
                budgets_hold = false;
                return;
            }
            sample_page = capture.page;
            sink += capture.page->rows().size();
        }
    });

    // 4. Steady FilesPaneGrid render on a 140x40 HeadlessTerminal backed by the 100k-row page
    // (before view-change revokes sorted_stamp).
    ckv::term::HeadlessTerminal terminal(ckv::Size{140, 40}, ckv::term::headless_no_graphics_profile());
    BenchManualClock clock;
    ckv::ui::Application app(terminal, clock);
    FilesPaneGridModel grid_model;
    grid_model.set_viewport(static_cast<int>(kViewportRows), 140);
    if (!sample_page || !grid_model.publish(sample_page, true)) {
        std::fprintf(stderr, "files_bench: grid_model.publish failed\n");
        budgets_hold = false;
    }
    auto grid = std::make_unique<ckv::widgets::CellGrid>();
    grid->set_model(grid_model);
    grid->set_bounds(ckv::Rect{0, 0, 140, 40});
    app.root().add_child(std::move(grid));
    app.step(0);

    bench.run("files_grid_steady_render_100k", 250, [&] {
        app.root().invalidate();
        terminal.clear_written();
        app.step(clock.now_nanos());
        if (app.last_compose_cells_touched() > kFrameCellsBudget || !terminal.written_bytes().empty()) {
            std::fprintf(stderr, "files_bench: steady render budget failed (cells=%zu bytes=%zu)\n",
                         app.last_compose_cells_touched(), terminal.written_bytes().size());
            budgets_hold = false;
            return;
        }
        sink += app.composed_surface().size().width;
    });

    // 5. Filter 100,000 entries by substring ("042") via IndexedPaneDataJob.
    std::uint64_t filtered_rows = 0;
    bench.run("files_index_filter_100k", 1, [&] {
        if (!sorted_data) {
            budgets_hold = false;
            return;
        }
        PaneView filtered = sorted_view;
        filtered.display_substring = "042";
        const auto req = pane.begin_view_change(sorted_stamp, filtered);
        if (!req.value()) {
            std::fprintf(stderr, "files_bench: begin_view_change(filter) failed\n");
            budgets_hold = false;
            return;
        }
        IndexedPaneDataJob job(sorted_data, *req.value(), filtered);
        IndexedPaneDataJobState state = job.state();
        while (state == IndexedPaneDataJobState::Sorting || state == IndexedPaneDataJobState::Indexing ||
               state == IndexedPaneDataJobState::CountingMarks) {
            state = job.advance();
        }
        const auto result = job.completed();
        if (state != IndexedPaneDataJobState::Complete || !result.value() ||
            (*result.value())->visible_rows() == 0 || (*result.value())->visible_rows() >= kEntryCount) {
            std::fprintf(stderr, "files_bench: filter failed (state=%d)\n", static_cast<int>(state));
            budgets_hold = false;
            return;
        }
        filtered_rows = (*result.value())->visible_rows();
        sink += static_cast<std::size_t>(filtered_rows);
    });

    // 6. Operation-recovery overlap assessment across a 1,000-journal scan.
    const auto target_source = synthetic_identity(scope, 42);
    const auto target_dest = synthetic_identity(scope, 99);
    const auto scan = thousand_journal_scan(target_source, target_dest);
    FrozenInventoryBuilder inv_builder(InventoryMountPolicy::DirectObjects, InventoryLinkPolicy::PreserveLinks);
    const auto target_path = NativeAbsolutePath::from_posix("/bench/file-000042.dat");
    if (!target_path.value() ||
        !inv_builder.add(FrozenInventoryEntry{*target_path.value(), target_source, ObjectKind::RegularFile, {}, 128,
                                              LinkTraversalEvidence::NotLink, std::nullopt, {}}) ||
        !inv_builder.seal()) {
        budgets_hold = false;
    }
    const auto inventory = inv_builder.completed();
    if (!inventory.value())
        budgets_hold = false;

    bench.run("files_recovery_overlap_1000", 200, [&] {
        if (!inventory.value()) {
            budgets_hold = false;
            return;
        }
        const auto report = assess_operation_recovery_overlap(scan, **inventory.value(), target_dest, std::nullopt);
        if (!report.has_conflict() || report.conflicts.size() != 1U) {
            budgets_hold = false;
            return;
        }
        sink += report.conflicts.size();
    });

    const std::uint64_t rss_kib = peak_rss_kib();
    std::printf(
        "budgets entries=%llu filtered=%llu index_bytes=%llu/%llu peak_backing_bytes=%llu peak_rss_kib=%llu "
        "frame=%zu/%zu cells idle-output=0 bytes\n",
        static_cast<unsigned long long>(kEntryCount), static_cast<unsigned long long>(filtered_rows),
        static_cast<unsigned long long>(index_bytes_after_scan), static_cast<unsigned long long>(kIndexBytesBudget),
        static_cast<unsigned long long>(stats->peak_bytes), static_cast<unsigned long long>(rss_kib),
        app.last_compose_cells_touched(), kFrameCellsBudget);
    std::printf("checksum %zu\n", sink);
    if (!budgets_hold)
        std::fputs("files benchmark budget failure: a deterministic bound or 100k-entry contract regressed\n", stderr);
    return budgets_hold && sink != 0 ? 0 : 1;
}
