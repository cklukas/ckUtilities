// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WP-07 deterministic model and decoder mutation/fuzzing suite covering
// ck-files settings, exact location text/completion codecs, operation
// journals, native recovery evidence, archive recovery inspection, and
// reconciliation overlap assessment.
#include "files_location_text.hpp"
#include "files_operation_recovery.hpp"
#include "files_settings.hpp"
#include "files_volume_archive.hpp"
#include "frozen_inventory.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "operation_journal.hpp"
#include "cvision/testing/cktest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ck::files;
using namespace ck::vision;

struct SplitMix64 {
    std::uint64_t state;
    std::uint64_t next() noexcept {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
    std::size_t range(std::size_t bound) noexcept { return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound); }
    std::uint8_t byte() noexcept { return static_cast<std::uint8_t>(next() & 0xffU); }
};

std::string mutate_text(SplitMix64 &rng, std::string sample) {
    const std::size_t ops = 1U + rng.range(5U);
    for (std::size_t op = 0; op < ops; ++op) {
        const std::size_t kind = rng.range(7U);
        if (kind == 0 && !sample.empty()) {
            sample[rng.range(sample.size())] ^= static_cast<char>(1U << rng.range(8U));
        } else if (kind == 1 && !sample.empty()) {
            sample.resize(rng.range(sample.size() + 1U));
        } else if (kind == 2 && sample.size() < 4096U) {
            const std::size_t at = rng.range(sample.size() + 1U);
            const char inject = static_cast<char>(rng.byte());
            sample.insert(sample.begin() + static_cast<std::ptrdiff_t>(at), inject);
        } else if (kind == 3 && !sample.empty()) {
            const std::size_t at = rng.range(sample.size());
            const std::size_t len = 1U + rng.range(std::min<std::size_t>(16U, sample.size() - at));
            sample.erase(at, len);
        } else if (kind == 4 && sample.size() < 4096U) {
            static constexpr std::string_view kTokens[] = {
                "\\xFF", "\\u{D800}", "\\u{110000}", "\\\"", "\\\\", "\0", "\r\n",
                "BOOKMARK 0 W ", "BOOKMARK 0 P ", "VIEW 99 99 99 99 99 99 ", "END\n",
                "\\\\?\\UNC\\srv\\share\\", "/../", "[\"/", "\", \""};
            const auto token = kTokens[rng.range(std::size(kTokens))];
            const std::size_t at = rng.range(sample.size() + 1U);
            sample.insert(at, token.data(), token.size());
        } else if (kind == 5 && !sample.empty()) {
            sample[rng.range(sample.size())] = '\0';
        } else if (kind == 6 && sample.size() < 2048U) {
            sample.append(sample);
        }
    }
    return sample;
}

std::vector<std::byte> mutate_bytes(SplitMix64 &rng, std::vector<std::byte> sample) {
    const std::size_t ops = 1U + rng.range(6U);
    for (std::size_t op = 0; op < ops; ++op) {
        const std::size_t kind = rng.range(6U);
        if (kind == 0 && !sample.empty()) {
            const std::size_t i = rng.range(sample.size());
            sample[i] = static_cast<std::byte>(static_cast<std::uint8_t>(sample[i]) ^ (1U << rng.range(8U)));
        } else if (kind == 1 && !sample.empty()) {
            sample.resize(rng.range(sample.size() + 1U));
        } else if (kind == 2 && sample.size() < 8192U) {
            const std::size_t at = rng.range(sample.size() + 1U);
            sample.insert(sample.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::byte>(rng.byte()));
        } else if (kind == 3 && !sample.empty()) {
            const std::size_t at = rng.range(sample.size());
            const std::size_t len = 1U + rng.range(std::min<std::size_t>(32U, sample.size() - at));
            sample.erase(sample.begin() + static_cast<std::ptrdiff_t>(at),
                         sample.begin() + static_cast<std::ptrdiff_t>(at + len));
        } else if (kind == 4 && sample.size() >= 4U) {
            const std::size_t at = rng.range(sample.size() - 3U);
            const std::uint8_t fill = (rng.next() & 1U) ? 0xffU : 0x00U;
            for (std::size_t k = 0; k < 4U; ++k)
                sample[at + k] = static_cast<std::byte>(fill);
        } else if (kind == 5 && sample.size() < 4096U) {
            const auto old_size = sample.size();
            sample.resize(old_size + 1U + rng.range(64U));
            for (std::size_t i = old_size; i < sample.size(); ++i)
                sample[i] = static_cast<std::byte>(rng.byte());
        }
    }
    return sample;
}

std::vector<std::byte> sample_journal_bytes(FileOperation op, std::span<const std::byte> payload) {
    OperationJournalId id{};
    id.bytes[0] = std::byte{0x44};
    id.bytes[15] = std::byte{0x99};
    auto created = OperationJournal::create(op, id);
    if (!created.value())
        return {};
    auto &journal = **created.value();
    (void)journal.append({1, 0, OperationJournalState::Planned, {}});
    (void)journal.append({2, 0, OperationJournalState::Prepared,
                          std::vector<std::byte>(payload.begin(), payload.end())});
    (void)journal.append({3, 0, OperationJournalState::Copied,
                          std::vector<std::byte>(payload.begin(), payload.end())});
    const auto encoded = encode_operation_journal(journal);
    return encoded.value() ? *encoded.value() : std::vector<std::byte>{};
}

} // namespace

CK_TEST(fuzz_files_settings_decoder_survives_mutations_and_bounds) {
    FilesPersistentSettings seed_settings;
    seed_settings.views[0].sort = PaneSortKey::Size;
    seed_settings.views[0].descending = true;
    seed_settings.views[0].folders_first = false;
    seed_settings.views[0].show_hidden = true;
    seed_settings.views[0].display_substring = "report";
    const auto posix_root = NativeAbsolutePath::from_posix("/");
    const auto win_root = NativeAbsolutePath::from_windows(u"V:\\");
    CK_CHECK(posix_root.value() && win_root.value());
    if (!posix_root.value() || !win_root.value())
        return;
    const auto posix_loc = FilesPaneLocation::from_components(
        *posix_root.value(), {*NativeName::from_posix("workspace"), *NativeName::from_posix("alpha")},
        NativePathLimits{}, FilesPaneLocation::hard_components);
    const auto win_loc = FilesPaneLocation::from_components(
        *win_root.value(), {*NativeName::from_windows(u"Projects"), *NativeName::from_windows(u"beta")},
        NativePathLimits{}, FilesPaneLocation::hard_components);
    CK_CHECK(posix_loc.value() && win_loc.value());
    if (!posix_loc.value() || !win_loc.value())
        return;
    seed_settings.bookmarks[0].push_back(*posix_loc.value());
    seed_settings.bookmarks[1].push_back(*win_loc.value());

    const std::string valid = files_settings_text(seed_settings);
    const auto reparsed = parse_files_settings(valid);
    CK_CHECK(reparsed.has_value());
    CK_CHECK(reparsed->bookmarks[0].size() == 1U && reparsed->bookmarks[1].size() == 1U);

    // Oversized settings (> 1 MiB) must be rejected cleanly without allocating unbounded memory.
    std::string oversized(1024U * 1024U + 64U, 'A');
    CK_CHECK(!parse_files_settings(oversized).has_value());

    SplitMix64 rng{0x5A17F00D00000001ULL};
    for (int i = 0; i < 2000; ++i) {
        const std::string mutated = mutate_text(rng, valid);
        const auto decoded = parse_files_settings(mutated);
        if (decoded) {
            const std::string canonical = files_settings_text(*decoded);
            CK_CHECK(!canonical.empty());
            CK_CHECK(parse_files_settings(canonical).has_value());
        }
    }
}

CK_TEST(fuzz_files_location_text_and_completion_codecs_survive_mutations) {
    const std::array<std::string, 6> seeds{
        "/home/user/docs",
        "[\"/home/user\", \"work\", \"raw\\xFF\"]",
        "V:\\Users\\work\\notes",
        "[\"V:\\\\Users\", \"literal. \", \"\\u{D800}\"]",
        "[\"\\\\?\\UNC\\server\\share\", \"child\"]",
        "/tmp/partial",
    };

    SplitMix64 rng{0x5A17F00D00000002ULL};
    std::size_t accepted = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto &base = seeds[rng.range(seeds.size())];
        const std::string mutated = mutate_text(rng, base);
        const auto syntax = (i & 1) ? FilesLocationTextSyntax::ExactNames : FilesLocationTextSyntax::Path;
        const auto encoding = (i & 2) ? NameEncoding::WindowsUtf16 : NameEncoding::PosixBytes;

        const auto parsed = parse_files_location_text(mutated, syntax, encoding);
        if (parsed.value()) {
            ++accepted;
            const auto exact = format_files_exact_location(*parsed.value());
            CK_CHECK(exact.value() != nullptr);
            if (exact.value()) {
                const auto roundtrip =
                    parse_files_location_text(*exact.value(), FilesLocationTextSyntax::ExactNames, encoding);
                CK_CHECK(roundtrip.value() != nullptr && *roundtrip.value() == *parsed.value());
            }
        }
        (void)parse_files_location_completion(mutated, syntax, encoding);
    }
    CK_CHECK(accepted > 0);
}

CK_TEST(fuzz_operation_journal_and_native_recovery_evidence_decoders) {
    const AdapterScope scope;
    const auto make_id = [&](std::uint64_t obj) {
        return *NativeDurableIdentity::from_live(
            ObjectIdentity{scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                           NativeIdentifier::from_u64(obj), {}});
    };
    const auto dest_dir = NativeAbsolutePath::from_posix("/dest/dir");
    CK_CHECK(dest_dir.value() != nullptr);
    const NativeOperationRecoveryEvidence seed_ev{
        *NativeName::from_posix(".ck-stage-fuzz"),
        *NativeName::from_posix("target.bin"),
        make_id(10),
        make_id(20),
        make_id(30),
        make_id(40),
        *dest_dir.value(),
        std::array<std::byte, 32>{std::byte{0xab}},
        make_id(50),
        StagingNamespace::Named,
        *NativeName::from_posix("source.bin"),
    };
    const auto encoded_ev = encode_native_operation_recovery_evidence(seed_ev);
    CK_CHECK(encoded_ev.value() != nullptr);
    const auto decoded_ev = decode_native_operation_recovery_evidence(*encoded_ev.value());
    CK_CHECK(decoded_ev.value() != nullptr);

    const auto valid_journal = sample_journal_bytes(FileOperation::Move, *encoded_ev.value());
    CK_CHECK(!valid_journal.empty());
    const auto decoded_journal = decode_operation_journal(valid_journal);
    CK_CHECK(decoded_journal.value() != nullptr && decoded_journal.value()->journal != nullptr);

    SplitMix64 rng{0x5A17F00D00000003ULL};
    for (int i = 0; i < 2000; ++i) {
        const auto mutated_ev = mutate_bytes(rng, *encoded_ev.value());
        (void)decode_native_operation_recovery_evidence(mutated_ev);

        const auto mutated_journal = mutate_bytes(rng, valid_journal);
        const auto j = decode_operation_journal(mutated_journal);
        if (j.value() && j.value()->journal) {
            for (const auto &rec : j.value()->journal->records())
                (void)decode_native_operation_recovery_evidence(rec.evidence);
        }
    }
}

CK_TEST(fuzz_volume_archive_recovery_inspector_handles_corrupt_journals) {
    std::vector<std::byte> archive_evidence;
    const auto append_u32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            archive_evidence.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    };
    const auto append_u64 = [&](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            archive_evidence.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    };
    const auto append_str = [&](std::string_view text) {
        append_u32(static_cast<std::uint32_t>(text.size()));
        for (char c : text)
            archive_evidence.push_back(static_cast<std::byte>(c));
    };
    archive_evidence.push_back(std::byte{'C'});
    archive_evidence.push_back(std::byte{'K'});
    archive_evidence.push_back(std::byte{'A'});
    archive_evidence.push_back(std::byte{'J'});
    append_u32(2);
    append_u64(77);
    append_str("vol-id");
    append_str("/mnt/vol");
    append_str("backup.ckarchive");
    archive_evidence.push_back(std::byte{1});
    append_str("/dest");
    archive_evidence.push_back(std::byte{1});
    append_str("/dest/.backup.cktmp");
    archive_evidence.push_back(std::byte{1});
    for (int b = 0; b < 32; ++b)
        archive_evidence.push_back(static_cast<std::byte>(b));

    const auto valid_archive_journal = sample_journal_bytes(FileOperation::Copy, archive_evidence);
    CK_CHECK(!valid_archive_journal.empty());

    SplitMix64 rng{0x5A17F00D00000004ULL};
    for (int i = 0; i < 1500; ++i) {
        const auto mutated = mutate_bytes(rng, valid_archive_journal);
        const auto decoded = decode_operation_journal(mutated);
        if (decoded.value() && decoded.value()->journal) {
            const auto report = inspect_volume_archive_recovery(*decoded.value()->journal);
            if (report.value()) {
                (void)report.value()->evidence.display_destination_folder();
            }
        }
    }
}

CK_TEST(fuzz_operation_recovery_overlap_fails_closed_under_adversarial_scans) {
    const AdapterScope scope;
    const auto make_live = [&](std::uint64_t obj) {
        return ObjectIdentity{scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                              NativeIdentifier::from_u64(obj), {}};
    };
    const auto src_id = make_live(100);
    const auto dst_id = make_live(200);
    const auto src_parent_id = make_live(50);

    const auto src_path = NativeAbsolutePath::from_posix("/fuzz/src.bin");
    CK_CHECK(src_path.value() != nullptr);
    FrozenInventoryBuilder builder(InventoryMountPolicy::DirectObjects, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(builder.add(FrozenInventoryEntry{*src_path.value(), src_id, ObjectKind::RegularFile, {}, 64,
                                              LinkTraversalEvidence::NotLink, std::nullopt, {}}));
    CK_CHECK(builder.seal());
    const auto inv = builder.completed();
    CK_CHECK(inv.value() != nullptr);

    SplitMix64 rng{0x5A17F00D00000005ULL};
    for (int trial = 0; trial < 1000; ++trial) {
        FilesOperationRecoveryScan scan;
        const std::size_t count = 1U + rng.range(12U);
        bool injected_untrusted = false;
        bool injected_overlap = false;

        for (std::size_t j = 0; j < count; ++j) {
            FilesOperationRecoveryItem item;
            item.display_name = ".ck-fuzz-journal-" + std::to_string(j);
            item.kind = FilesOperationJournalKind::Transfer;
            const std::size_t mode = rng.range(7U);
            if (mode == 0) {
                item.error = UnavailableObservation{ObservationState::Unsupported,
                                                    {ErrorDomain::Adapter, 1, "fuzz.corrupt"}};
                injected_untrusted = true;
            } else if (mode == 1) {
                FilesOperationRecoveryReport rep;
                rep.journal_id.bytes[0] = static_cast<std::byte>(j);
                rep.inspection.report.has_untrusted_tail = true;
                rep.inspection.report.requires_reconciliation = true;
                item.report = std::move(rep);
                injected_untrusted = true;
            } else if (mode == 2) {
                FilesOperationRecoveryReport rep;
                rep.journal_id.bytes[0] = static_cast<std::byte>(j);
                rep.inspection.has_untrusted_evidence = true;
                rep.inspection.report.requires_reconciliation = true;
                item.report = std::move(rep);
                injected_untrusted = true;
            } else if (mode == 3) {
                FilesOperationRecoveryReport rep;
                rep.journal_id.bytes[0] = static_cast<std::byte>(j);
                OperationRecoveryItem rec;
                rec.item = 0;
                rec.state = OperationJournalState::Prepared;
                rec.action = OperationRecoveryAction::InspectUnpublishedStaging;
                rec.requires_reconciliation = true;
                rep.inspection.report.items.push_back(rec);
                rep.inspection.report.requires_reconciliation = true;
                item.report = std::move(rep);
                injected_untrusted = true;
            } else if (mode == 4) {
                FilesOperationRecoveryReport rep;
                rep.journal_id.bytes[0] = static_cast<std::byte>(j);
                OperationRecoveryItem rec;
                rec.item = 0;
                rec.state = OperationJournalState::DestinationCommitted;
                rec.action = OperationRecoveryAction::VerifyDestinationBeforeSourceRetirement;
                rec.requires_reconciliation = true;
                rep.inspection.report.items.push_back(rec);
                rep.inspection.report.requires_reconciliation = true;
                NativeOperationRecoveryArtifact art;
                art.item = 0;
                art.evidence = NativeOperationRecoveryEvidence{
                    *NativeName::from_posix("st"), *NativeName::from_posix("fn"),
                    *NativeDurableIdentity::from_live(src_parent_id),
                    *NativeDurableIdentity::from_live(src_id),
                    *NativeDurableIdentity::from_live(make_live(999)),
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    StagingNamespace::Named, *NativeName::from_posix("src")};
                rep.inspection.artifacts.push_back(std::move(art));
                item.report = std::move(rep);
                injected_overlap = true;
            } else if (mode == 5) {
                FilesOperationRecoveryReport rep;
                rep.journal_id.bytes[0] = static_cast<std::byte>(j);
                OperationRecoveryItem rec;
                rec.item = 0;
                rec.state = OperationJournalState::RetainedSource;
                rec.action = OperationRecoveryAction::IgnoreTerminal;
                rec.requires_reconciliation = false;
                rep.inspection.report.items.push_back(rec);
                NativeOperationRecoveryArtifact art;
                art.item = 0;
                art.evidence = NativeOperationRecoveryEvidence{
                    *NativeName::from_posix("st"), *NativeName::from_posix("fn"),
                    *NativeDurableIdentity::from_live(src_parent_id),
                    *NativeDurableIdentity::from_live(src_id),
                    *NativeDurableIdentity::from_live(dst_id),
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    StagingNamespace::Named, *NativeName::from_posix("src")};
                rep.inspection.artifacts.push_back(std::move(art));
                item.report = std::move(rep);
                injected_overlap = true;
            } else {
                FilesOperationRecoveryReport rep;
                rep.journal_id.bytes[0] = static_cast<std::byte>(j);
                OperationRecoveryItem rec;
                rec.item = 0;
                rec.state = OperationJournalState::Complete;
                rec.action = OperationRecoveryAction::IgnoreTerminal;
                rec.requires_reconciliation = false;
                rep.inspection.report.items.push_back(rec);
                NativeOperationRecoveryArtifact art;
                art.item = 0;
                art.evidence = NativeOperationRecoveryEvidence{
                    *NativeName::from_posix("st"), *NativeName::from_posix("fn"),
                    *NativeDurableIdentity::from_live(make_live(300 + j)),
                    *NativeDurableIdentity::from_live(make_live(400 + j)),
                    *NativeDurableIdentity::from_live(make_live(500 + j)),
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    StagingNamespace::Named, *NativeName::from_posix("other")};
                rep.inspection.artifacts.push_back(std::move(art));
                item.report = std::move(rep);
            }
            scan.journals.push_back(std::move(item));
        }

        const auto report = assess_operation_recovery_overlap(scan, **inv.value(), dst_id, src_parent_id);
        if (injected_untrusted) {
            CK_CHECK(report.has_conflict());
            CK_CHECK(report.has_untrusted_journal());
        } else if (injected_overlap) {
            CK_CHECK(report.has_conflict());
        } else {
            CK_CHECK(!report.has_conflict());
        }
    }
}
