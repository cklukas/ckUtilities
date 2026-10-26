// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_name.hpp"
#include "observation.hpp"
#include <filesystem>
#include <vector>
namespace ck::files {
struct NativePathLimits {
    std::size_t max_native_bytes = 256 * 1024;
    static constexpr std::size_t hard_native_bytes = 1024 * 1024;
};
// An exact absolute navigation address, not an object identity or a safe
// operation target. Earlier components follow host navigation semantics.
// No canonicalization, separator rewriting, cwd lookup, Unicode conversion,
// existence check, I/O, path-length guarantee or preservation authority.
class NativeAbsolutePath final {
public:
    NativeAbsolutePath(const NativeAbsolutePath&) = default;
    NativeAbsolutePath& operator=(const NativeAbsolutePath&) = default;
    NativeAbsolutePath(NativeAbsolutePath&& other) noexcept : value_(std::move(other.value_)) { other.clear(); }
    NativeAbsolutePath& operator=(NativeAbsolutePath&& other) noexcept {
        if (this != &other) { value_ = std::move(other.value_); other.clear(); }
        return *this;
    }
    static Observation<NativeAbsolutePath> from_posix(std::string_view bytes, NativePathLimits limits = {});
    static Observation<NativeAbsolutePath> from_windows(std::u16string_view units, NativePathLimits limits = {});
    static Observation<NativeAbsolutePath> from_host(const std::filesystem::path& path, NativePathLimits limits = {});
    // Append one exact native basename without parsing display text, changing
    // encoding, normalizing separators, or resolving links/dot components.
    Observation<NativeAbsolutePath> child_address(const NativeName& component,
        NativePathLimits limits = {}) const;
    // A foreign encoding is Unsupported, never silently transcoded. This
    // constructs an exact native host path; it performs no filesystem access.
    Observation<std::filesystem::path> host_path() const;
    // Lexical requested-address navigation only. Preserve the namespace and
    // native units; do not resolve dot components, links, mounts or identities.
    // Parent of a namespace root is that root. Invalid/moved values refuse.
    Observation<NativeAbsolutePath> parent_address() const;
    Observation<NativeAbsolutePath> namespace_root() const;
    // Return exact native components below an exact lexical ancestor. This is
    // a requested-address relation only: it performs no canonicalization,
    // link resolution, case folding or filesystem access.
    Observation<std::vector<NativeName>> relative_components_from(
        const NativeAbsolutePath& ancestor) const;
    bool is_valid() const noexcept;
    NameEncoding encoding() const noexcept;
    const std::string* posix_bytes() const noexcept;
    const std::u16string* windows_units() const noexcept;
    std::string display_text() const; // presentation only; never parse back
    friend bool operator==(const NativeAbsolutePath&, const NativeAbsolutePath&) = default;
private:
    void clear() noexcept {
        if (auto* bytes = std::get_if<std::string>(&value_)) bytes->clear();
        else std::get<std::u16string>(value_).clear();
    }
    explicit NativeAbsolutePath(std::string bytes) : value_(std::move(bytes)) {}
    explicit NativeAbsolutePath(std::u16string units) : value_(std::move(units)) {}
    std::variant<std::string, std::u16string> value_;
};
} // namespace ck::files
