// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_name.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ck::files {

// An identifier in a feature namespace, not a basename or operation path.
// Preserve separators, colon syntax, controls and invalid encoding literally.
class NativeFeatureIdentifier final {
public:
    static std::optional<NativeFeatureIdentifier> from_posix(std::string bytes);
    static std::optional<NativeFeatureIdentifier> from_windows(std::u16string units);
    NameEncoding encoding() const noexcept;
    const std::string* posix_bytes() const noexcept;
    const std::u16string* windows_units() const noexcept;
    std::string display_name() const;
    friend bool operator==(const NativeFeatureIdentifier&, const NativeFeatureIdentifier&) = default;
private:
    explicit NativeFeatureIdentifier(std::string bytes) : value_(std::move(bytes)) {}
    explicit NativeFeatureIdentifier(std::u16string units) : value_(std::move(units)) {}
    std::variant<std::string, std::u16string> value_;
};

struct InventoryLimits {
    std::size_t max_native_bytes = 1024 * 1024;
    std::size_t max_records = 16384;
    static constexpr std::size_t hard_native_bytes = 8 * 1024 * 1024;
    static constexpr std::size_t hard_records = 65536;
    bool valid() const noexcept;
};

enum class DataChannelKind { MainData, NamedData, ResourceFork, Opaque };
struct DataChannelInfo {
    NativeFeatureIdentifier identifier;
    DataChannelKind kind = DataChannelKind::Opaque;
    Observation<std::uint64_t> logical_size;
    Observation<std::uint64_t> allocated_size;
};

// Coverage is the exact native query family, never a claim that unrelated
// metadata/provider features are absent. An empty resource-fork catalog, for
// example, says nothing about other extended attributes or provider channels.
enum class DataChannelCoverage { Unknown, WindowsFileStreamInfo, MacResourceFork };
struct DataChannelCatalog {
    std::vector<DataChannelInfo> channels;
    DataChannelCoverage coverage = DataChannelCoverage::Unknown;
    Observation<ObjectIdentity> owner;
};

} // namespace ck::files
