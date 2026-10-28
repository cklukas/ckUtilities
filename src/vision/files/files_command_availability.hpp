// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <utility>

namespace ck::vision {

// One answer shared by command predicates and the user-facing status hint.
// The reason is deliberately operation-specific; callers must not replace it
// with a generic "not available" message.
struct FilesCommandAvailability {
    bool enabled = false;
    std::string reason;

    static FilesCommandAvailability available() { return {true, {}}; }
    static FilesCommandAvailability unavailable(std::string why) { return {false, std::move(why)}; }
};

} // namespace ck::vision
