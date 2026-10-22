// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "monitor.hpp"
#include <CoreFoundation/CoreFoundation.h>

namespace ck::sysmon {
// Driver snapshot keys are optional implementation details. Apply only to a
// unified-memory Metal device joined to this exact IORegistry entry.
void apply_apple_gpu_statistics(GpuSample& gpu, CFDictionaryRef properties,
                               std::optional<std::uint64_t> physical_memory);
} // namespace ck::sysmon
