// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_info.hpp"
#include "opened_entry.hpp"
#include "cvision/testing/cktest.hpp"

namespace {
using namespace ck::files;
StorageInfo windows(std::uint32_t flags, std::uint32_t tag = 0) {
    BasicMetadata metadata;
    metadata.windows_attributes = Observation<std::uint32_t>::available(flags);
    metadata.reparse_tag = Observation<std::uint32_t>::available(tag);
    return *storage_from_basic(metadata, StorageModel::WindowsHandleAttributes).value();
}
}
CK_TEST(storage_defaults_and_missing_native_bits_do_not_assert_absence) {
    CK_CHECK(StorageInfo{}.model == StorageModel::Unknown);
    CK_CHECK(reported_storage_flag(StorageInfo{}, StorageFeature::CompressionFlag).state() == ObservationState::Unknown);
    BasicMetadata metadata;
    CK_CHECK(storage_from_basic(metadata, StorageModel::Unknown).state() == ObservationState::Failed);
    const auto linux = storage_from_basic(metadata, StorageModel::LinuxStatx);
    CK_CHECK(linux.value() && linux.value()->flags.state() == ObservationState::Unknown);
    CK_CHECK(data_access_hint(*linux.value()).state() == ObservationState::Unsupported);
}
CK_TEST(storage_flag_validity_masks_and_native_failures_survive_normalization) {
    BasicMetadata metadata;
    metadata.linux_attributes = Observation<LinuxFileAttributes>::available({4, 0});
    auto info = *storage_from_basic(metadata, StorageModel::LinuxStatx).value();
    CK_CHECK(reported_storage_flag(info, StorageFeature::CompressionFlag).state() == ObservationState::Unknown);
    info.flags = Observation<NativeStorageFlags>::available({4, 4});
    CK_CHECK(*reported_storage_flag(info, StorageFeature::CompressionFlag).value() == ReportedFlag::Set);
    info.flags = Observation<NativeStorageFlags>::available({0, 4});
    CK_CHECK(*reported_storage_flag(info, StorageFeature::CompressionFlag).value() == ReportedFlag::Clear);
    const NativeError error{ErrorDomain::Posix, 13, "injected.storage"};
    metadata.linux_attributes = Observation<LinuxFileAttributes>::unavailable(ObservationState::Denied, error);
    const auto denied = storage_from_basic(metadata, StorageModel::LinuxStatx);
    const auto result = reported_storage_flag(*denied.value(), StorageFeature::CompressionFlag);
    CK_CHECK(result.state() == ObservationState::Denied && result.unavailable_detail()->error == error);
}
CK_TEST(storage_allocation_counts_do_not_establish_sparse_extents_clones_or_encryption) {
    BasicMetadata metadata;
    metadata.logical_size = Observation<std::uint64_t>::available(1000000);
    metadata.allocated_size = Observation<std::uint64_t>::available(4096);
    metadata.bsd_flags = Observation<std::uint32_t>::available(0);
    const auto info = storage_from_basic(metadata, StorageModel::DarwinStatFlags);
    CK_CHECK(reported_storage_flag(*info.value(), StorageFeature::SparseFlag).state() == ObservationState::Unsupported);
    CK_CHECK(reported_storage_flag(*info.value(), StorageFeature::EncryptionFlag).state() == ObservationState::Unsupported);
    CK_CHECK(info.value()->windows_compression.state() == ObservationState::Unsupported);
}
CK_TEST(windows_handle_ea_alias_is_not_an_enumeration_recall_on_open_flag) {
    const auto info = windows(0x40000);
    CK_CHECK(*data_access_hint(info).value() == DataAccessHint::NoRecallFlagReported);
    CK_CHECK(reported_storage_flag(info, static_cast<StorageFeature>(255)).state() == ObservationState::Failed);
    for (const auto bit : {0x1000U, 0x400000U})
        CK_CHECK(*data_access_hint(windows(bit)).value() == DataAccessHint::RecallFlagReported);
    for (const auto bit : {0x80000U, 0x100000U})
        CK_CHECK(*data_access_hint(windows(bit)).value() == DataAccessHint::ProviderIntentReported);
}
CK_TEST(provider_tags_are_not_generic_links_and_pin_intent_does_not_prove_local_content) {
    for (std::uint32_t variant = 0; variant < 16; ++variant)
        CK_CHECK(*data_access_hint(windows(0x400, 0x9000001aU | (variant << 12U))).value() == DataAccessHint::ProviderTagReported);
    for (const auto tag : {0x80000017U, 0x9000001cU, 0x80000018U})
        CK_CHECK(*data_access_hint(windows(0x400, tag)).value() == DataAccessHint::ProviderTagReported);
    CK_CHECK(*data_access_hint(windows(0x400, 0x80000042)).value() == DataAccessHint::OpaqueReparseReported);
    for (const auto tag : {0xa000000cU, 0xa0000003U})
        CK_CHECK(*data_access_hint(windows(0x400, tag)).value() == DataAccessHint::NoRecallFlagReported);
    auto unknown = windows(0x400);
    unknown.windows_reparse_tag = {};
    CK_CHECK(data_access_hint(unknown).state() == ObservationState::Unknown);
}
CK_TEST(provider_review_uses_reported_hints_and_preserves_unavailable_knowledge) {
    BasicMetadata metadata;
    CK_CHECK(provider_review_required(metadata, StorageModel::DarwinStatFlags).state() == ObservationState::Unknown);
    metadata.bsd_flags = Observation<std::uint32_t>::available(0x40000000);
    CK_CHECK(*provider_review_required(metadata, StorageModel::DarwinStatFlags).value());
    metadata.bsd_flags = Observation<std::uint32_t>::available(0);
    CK_CHECK(!*provider_review_required(metadata, StorageModel::DarwinStatFlags).value());
    metadata.windows_attributes = Observation<std::uint32_t>::available(0x1000);
    CK_CHECK(*provider_review_required(metadata, StorageModel::WindowsHandleAttributes).value());
}
