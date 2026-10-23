// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ck::files {

enum class NameEncoding { PosixBytes, WindowsUtf16, WindowsEaBytes };

// Presentation only, including for non-path native identifiers. Never parse
// these escaped UTF-8 strings back into operation names.
std::string display_native_text(std::string_view bytes);
std::string display_native_text(std::u16string_view units);

// One native directory-entry basename, never a path or display string.
// Both representations are supported on every host, including in fake adapters.
// Factory validation establishes component boundaries only. Destination-specific
// naming restrictions, length limits and collisions are separate capabilities.
class NativeName final {
public:
    // Reject empty names, NUL, '/' and the navigation entries '.' and '..'.
    // Backslash and non-UTF-8 bytes are legitimate POSIX name bytes.
    static std::optional<NativeName> from_posix(std::string bytes);
    // Reject empty names, NUL, separators and navigation entries. Preserve
    // unpaired surrogates; native names need not be well-formed Unicode text.
    static std::optional<NativeName> from_windows(std::u16string units);

    NameEncoding encoding() const noexcept;
    const std::string* posix_bytes() const noexcept;
    const std::u16string* windows_units() const noexcept;

    // Valid UTF-8 for presentation only. Escape literal backslashes, invalid
    // encoding, terminal/bidi controls, line separators and noncharacters.
    // No normalization or replacement characters are introduced. Never parse
    // this string to select an object or reconstruct a native path. Prepare
    // display text outside rendering/provider callbacks.
    std::string display_name() const;

    friend bool operator==(const NativeName&, const NativeName&) = default;

private:
    explicit NativeName(std::string bytes);
    explicit NativeName(std::u16string units);
    std::variant<std::string, std::u16string> value_;
};

} // namespace ck::files
