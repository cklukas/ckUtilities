// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "cvision/core/version.hpp"

namespace ck::host {

// The ckUtilities release these tools belong to, from the build.
inline constexpr std::string_view kSuiteVersion = CK_UTILITIES_VERSION;

inline constexpr std::string_view kCopyrightNotice =
    "Copyright (c) 2026 C. Klukas. All rights reserved.";

// "ck-todo 0.1.1 (ckVision 1.3.0)" -- what --version prints.
inline std::string version_line(std::string_view tool) {
    return std::string(tool) + " " + std::string(kSuiteVersion) + " (ckVision " +
           std::string(ckv::version_string()) + ")";
}

// An About box body: the tool's description, then the suite and copyright.
inline std::string about_text(std::string description) {
    if (!description.empty()) description += "\n\n";
    description += "Part of CK Utilities " + std::string(kSuiteVersion) + ".\n";
    description += kCopyrightNotice;
    return description;
}

}  // namespace ck::host
