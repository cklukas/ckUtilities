// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "opened_entry.hpp"
#include <map>
#include <stdexcept>

namespace ck::testing {
struct SimulatedFeatureLess {
    bool operator()(const ck::files::NativeFeatureIdentifier& a, const ck::files::NativeFeatureIdentifier& b) const noexcept {
        if (a.encoding() != b.encoding()) return a.encoding() < b.encoding();
        if (a.posix_bytes()) return *a.posix_bytes() < *b.posix_bytes();
        if (a.windows_units()) return *a.windows_units() < *b.windows_units();
        return *a.windows_ea_bytes() < *b.windows_ea_bytes();
    }
};
// Explicit query replies, not emulation of a host filesystem or proof of native
// preservation. Unconfigured families/keys remain Unsupported, never absent.
// Owners, coverage, malformed payloads and per-field unavailable observations
// are preserved literally so consumers can be tested against inconsistent data.
struct SimulatedInventory {
    template<class T> using Reply = ck::files::Observation<T>;
    std::optional<Reply<ck::files::StorageInfo>> storage;
    std::optional<Reply<ck::files::FileDataLayout>> layout;
    std::optional<Reply<ck::files::LinkMetadata>> link;
    std::optional<Reply<ck::files::DataChannelCatalog>> channels;
    std::optional<Reply<ck::files::AttributeCatalog>> attributes;
    std::map<ck::files::NativeFeatureIdentifier, Reply<ck::files::AttributeValue>, SimulatedFeatureLess> values;
    std::map<ck::files::SecurityPart, Reply<ck::files::NativeSecurityValue>> security;

    struct Budget {
        std::size_t bytes = 0, records = 0;
        bool add(std::size_t size, std::size_t count, ck::files::InventoryLimits limits) noexcept {
            if (bytes > limits.max_native_bytes || records > limits.max_records) return false;
            if (size > limits.max_native_bytes - bytes || count > limits.max_records - records) return false;
            bytes += size; records += count; return true;
        }
    };
    static std::size_t bytes(const ck::files::NativeFeatureIdentifier& id) noexcept {
        if (id.posix_bytes()) return id.posix_bytes()->size();
        if (id.windows_units()) {
            // Identifiers supplied by a fixture are bounded before multiplication.
            const auto units = id.windows_units()->size();
            return units > ck::files::InventoryLimits::hard_native_bytes / 2 ?
                ck::files::InventoryLimits::hard_native_bytes + 1 : units * 2;
        }
        return id.windows_ea_bytes()->size();
    }
    static bool fits(const ck::files::DataChannelCatalog& catalog, ck::files::InventoryLimits limits) noexcept {
        Budget budget;
        for (const auto& channel : catalog.channels) if (!budget.add(bytes(channel.identifier), 1, limits)) return false;
        return true;
    }
    static bool fits(const ck::files::AttributeCatalog& catalog, ck::files::InventoryLimits limits) noexcept {
        Budget budget;
        for (const auto& attribute : catalog.attributes) if (!budget.add(bytes(attribute.identifier), 1, limits)) return false;
        return true;
    }
    static bool fits(const ck::files::AttributeValue& value, ck::files::InventoryLimits limits) noexcept {
        // Query byte budgets constrain returned values, independently of the key.
        return value.bytes.size() <= limits.max_native_bytes;
    }
    static bool fits(const ck::files::FileDataLayout& value, ck::files::InventoryLimits limits) noexcept {
        return value.ranges.size() <= limits.max_records && value.ranges.size() <= limits.max_native_bytes / sizeof(ck::files::FileDataRange);
    }
    static bool fits(const ck::files::NativeSecurityValue& value, ck::files::InventoryLimits limits) noexcept {
        return value.bytes.size() <= limits.max_native_bytes;
    }
    static bool fits(const ck::files::LinkMetadata& value, ck::files::InventoryLimits limits) noexcept {
        if (const auto* posix = std::get_if<ck::files::PosixSymlinkData>(&value.payload)) return posix->target.size() <= limits.max_native_bytes;
        if (const auto* windows = std::get_if<ck::files::WindowsReparseData>(&value.payload)) return windows->raw.size() <= limits.max_native_bytes;
        return true; // malformed payloads are deliberately available to consumer tests
    }
    // One node has a cumulative hard budget, including keys and decoded names,
    // rather than one independent 8 MiB allocation per configured value.
    void validate() const {
        using namespace ck::files;
        constexpr InventoryLimits hard{InventoryLimits::hard_native_bytes, InventoryLimits::hard_records};
        Budget budget;
        const auto add = [&](std::size_t size, std::size_t count = 1) {
            if (!budget.add(size, count, hard)) throw std::length_error("simulated inventory limit");
        };
        if (storage) add(0);
        if (layout) {
            add(0);
            if (const auto* value = layout->value()) for (const auto& range : value->ranges) { (void)range; add(sizeof(FileDataRange)); }
        }
        if (link) {
            add(0);
            if (const auto* value = link->value()) {
                if (const auto* posix = std::get_if<PosixSymlinkData>(&value->payload)) add(posix->target.size(), 0);
                if (const auto* windows = std::get_if<WindowsReparseData>(&value->payload)) {
                    add(windows->raw.size(), 0);
                    if (windows->names) {
                        for (const auto* text : {&windows->names->substitute, &windows->names->print}) {
                            if (text->size() > hard.max_native_bytes / 2) throw std::length_error("simulated link name limit");
                            add(text->size() * 2, 0);
                        }
                    }
                }
            }
        }
        if (channels) {
            add(0);
            if (const auto* value = channels->value()) for (const auto& channel : value->channels) add(bytes(channel.identifier));
        }
        if (attributes) {
            add(0);
            if (const auto* value = attributes->value()) for (const auto& attribute : value->attributes) add(bytes(attribute.identifier));
        }
        for (const auto& [key, reply] : values) {
            add(bytes(key));
            if (const auto* value = reply.value()) { add(bytes(value->identifier), 0); add(value->bytes.size(), 0); }
        }
        for (const auto& [part, reply] : security) {
            if (!valid_security_part(part)) throw std::invalid_argument("simulated security part");
            add(0);
            if (const auto* value = reply.value()) add(value->bytes.size(), 0);
        }
    }
};
} // namespace ck::testing
