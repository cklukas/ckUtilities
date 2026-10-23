// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_info.hpp"
#include <limits>
#include <optional>

namespace ck::files {
namespace {
template <class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
std::optional<std::uint64_t> feature_mask(StorageModel model, StorageFeature feature) {
    if (model == StorageModel::WindowsHandleAttributes) {
        switch (feature) {
        case StorageFeature::CompressionFlag: return 0x800;
        case StorageFeature::EncryptionFlag: return 0x4000;
        case StorageFeature::SparseFlag: return 0x200;
        case StorageFeature::OfflineFlag: return 0x1000;
        case StorageFeature::RecallOnDataAccessFlag: return 0x400000;
        case StorageFeature::PinnedIntentFlag: return 0x80000;
        case StorageFeature::UnpinnedIntentFlag: return 0x100000;
        case StorageFeature::IntegrityStreamFlag: return 0x8000;
        case StorageFeature::NoScrubFlag: return 0x20000;
        case StorageFeature::ReparseFlag: return 0x400;
        case StorageFeature::HiddenFlag: return 2;
        default: return std::nullopt;
        }
    }
    if (model == StorageModel::DarwinStatFlags) {
        switch (feature) {
        case StorageFeature::CompressionFlag: return 0x20;
        case StorageFeature::ImmutableFlag: return 0x2 | 0x20000;
        case StorageFeature::AppendOnlyFlag: return 0x4 | 0x40000;
        case StorageFeature::DatalessFlag: return 0x40000000;
        case StorageFeature::HiddenFlag: return 0x8000;
        default: return std::nullopt;
        }
    }
    if (model == StorageModel::LinuxStatx) {
        switch (feature) {
        case StorageFeature::CompressionFlag: return 0x4;
        case StorageFeature::EncryptionFlag: return 0x800;
        case StorageFeature::ImmutableFlag: return 0x10;
        case StorageFeature::AppendOnlyFlag: return 0x20;
        case StorageFeature::VerityFlag: return 0x100000;
        case StorageFeature::DaxFlag: return 0x200000;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}
}
Observation<StorageInfo> storage_from_basic(const BasicMetadata& metadata, StorageModel model) {
    using Result = Observation<StorageInfo>;
    if (model != StorageModel::LinuxStatx && model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes)
        return error<StorageInfo>(ObservationState::Failed, AdapterError::InvalidIdentifier, "storage.model");
    StorageInfo result;
    result.model = model;
    result.owner = metadata.identity;
    result.kind = metadata.kind;
    result.logical_size = metadata.logical_size;
    result.allocated_size = metadata.allocated_size;
    result.windows_reparse_tag = metadata.reparse_tag;
    result.windows_compression = error<WindowsCompressionDetails>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "storage.compression_details");
    if (model == StorageModel::LinuxStatx) {
        if (metadata.linux_attributes.value())
            result.flags = Observation<NativeStorageFlags>::available({metadata.linux_attributes.value()->value, metadata.linux_attributes.value()->known_mask});
        else { const auto& e = *metadata.linux_attributes.unavailable_detail(); result.flags = Observation<NativeStorageFlags>::unavailable(e.state, e.error); }
    } else {
        const auto& native = model == StorageModel::DarwinStatFlags ? metadata.bsd_flags : metadata.windows_attributes;
        if (native.value()) result.flags = Observation<NativeStorageFlags>::available({*native.value(), std::numeric_limits<std::uint32_t>::max()});
        else { const auto& e = *native.unavailable_detail(); result.flags = Observation<NativeStorageFlags>::unavailable(e.state, e.error); }
    }
    return Result::available(std::move(result));
}
Observation<ReportedFlag> reported_storage_flag(const StorageInfo& info, StorageFeature feature) {
    using Result = Observation<ReportedFlag>;
    if (feature <= StorageFeature::Unknown || feature > StorageFeature::HiddenFlag)
        return error<ReportedFlag>(ObservationState::Failed, AdapterError::InvalidIdentifier, "storage.feature");
    if (info.model != StorageModel::LinuxStatx && info.model != StorageModel::DarwinStatFlags && info.model != StorageModel::WindowsHandleAttributes)
        return error<ReportedFlag>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "storage.model");
    const auto mask = feature_mask(info.model, feature);
    if (!mask) return error<ReportedFlag>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "storage.feature_mapping");
    if (!info.flags.value()) { const auto& e = *info.flags.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    const auto known = info.flags.value()->known_mask & *mask;
    if ((info.flags.value()->value & known) != 0) return Result::available(ReportedFlag::Set);
    if (known != *mask) return error<ReportedFlag>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "storage.flag_validity");
    return Result::available(ReportedFlag::Clear);
}
Observation<DataAccessHint> data_access_hint(const StorageInfo& info) {
    using Result = Observation<DataAccessHint>;
    if (info.model == StorageModel::DarwinStatFlags) {
        const auto flag = reported_storage_flag(info, StorageFeature::DatalessFlag);
        if (!flag.value()) { const auto& e = *flag.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available(*flag.value() == ReportedFlag::Set ? DataAccessHint::RecallFlagReported : DataAccessHint::NoRecallFlagReported);
    }
    if (info.model != StorageModel::WindowsHandleAttributes)
        return error<DataAccessHint>(info.model == StorageModel::Unknown ? ObservationState::Unknown : ObservationState::Unsupported,
            AdapterError::UnsupportedQuery, "storage.provider_query");
    const auto offline = reported_storage_flag(info, StorageFeature::OfflineFlag);
    const auto recall = reported_storage_flag(info, StorageFeature::RecallOnDataAccessFlag);
    if ((offline.value() && *offline.value() == ReportedFlag::Set) || (recall.value() && *recall.value() == ReportedFlag::Set))
        return Result::available(DataAccessHint::RecallFlagReported);
    if (!offline.value()) { const auto& e = *offline.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (!recall.value()) { const auto& e = *recall.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    const auto reparse = reported_storage_flag(info, StorageFeature::ReparseFlag);
    if (!reparse.value()) { const auto& e = *reparse.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (*reparse.value() == ReportedFlag::Set) {
        if (!info.windows_reparse_tag.value()) { const auto& e = *info.windows_reparse_tag.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto tag = *info.windows_reparse_tag.value();
        if ((tag & 0xffff0fffU) == 0x9000001aU || tag == 0x80000017U || tag == 0x9000001cU || tag == 0x80000018U)
            return Result::available(DataAccessHint::ProviderTagReported);
        if (tag != 0xa000000cU && tag != 0xa0000003U)
            return Result::available(DataAccessHint::OpaqueReparseReported);
    }
    const auto pinned = reported_storage_flag(info, StorageFeature::PinnedIntentFlag);
    const auto unpinned = reported_storage_flag(info, StorageFeature::UnpinnedIntentFlag);
    if (!pinned.value()) { const auto& e = *pinned.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (!unpinned.value()) { const auto& e = *unpinned.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    return Result::available(*pinned.value() == ReportedFlag::Set || *unpinned.value() == ReportedFlag::Set ?
        DataAccessHint::ProviderIntentReported : DataAccessHint::NoRecallFlagReported);
}
Observation<bool> provider_review_required(const BasicMetadata& metadata, StorageModel model) {
    const auto storage = storage_from_basic(metadata, model);
    if (!storage.value()) { const auto& e = *storage.unavailable_detail(); return Observation<bool>::unavailable(e.state, e.error); }
    const auto hint = data_access_hint(*storage.value());
    if (!hint.value()) { const auto& e = *hint.unavailable_detail(); return Observation<bool>::unavailable(e.state, e.error); }
    return Observation<bool>::available(*hint.value() != DataAccessHint::NoRecallFlagReported);
}
} // namespace ck::files
