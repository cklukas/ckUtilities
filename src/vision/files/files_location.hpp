// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_path.hpp"
#include <span>
#include <vector>
namespace ck::vision {
// Owner-free requested route: an absolute root address followed by exact native
// components. The root may have normal host path semantics; each descendant is
// resolved through a held cursor. This is neither an object identity nor a flat
// pathname suitable for reopening. In particular, Windows literal components
// must never be appended to an ordinary Win32 path and reopened.
class FilesPaneLocation final {
  public:
    explicit FilesPaneLocation(ck::files::NativeAbsolutePath root);
    // Validate and adopt a complete route in linear time; never partial output.
    static ck::files::Observation<FilesPaneLocation> from_components(ck::files::NativeAbsolutePath root,
                                                                     std::vector<ck::files::NativeName> components,
                                                                     ck::files::NativePathLimits limits,
                                                                     std::size_t max_components);
    const ck::files::NativeAbsolutePath &root_address() const noexcept { return root_; }
    std::span<const ck::files::NativeName> components() const noexcept { return components_; }
    std::size_t native_bytes() const noexcept { return bytes_; }
    std::string display_text() const; // escaped presentation only, never parse back
    // Requested-address navigation, not retained ancestor identity. Exact
    // components remain separate and are replayed through held cursors.
    ck::files::Observation<FilesPaneLocation> parent() const;
    ck::files::Observation<FilesPaneLocation> namespace_root() const;
    ck::files::Observation<FilesPaneLocation>
    descend(const ck::files::NativeName &name, ck::files::NativePathLimits limits, std::size_t max_components) const;
    static constexpr std::size_t hard_components = 4096;
    friend bool operator==(const FilesPaneLocation &, const FilesPaneLocation &) = default;

  private:
    ck::files::NativeAbsolutePath root_;
    std::vector<ck::files::NativeName> components_;
    std::size_t bytes_;
};
} // namespace ck::vision
