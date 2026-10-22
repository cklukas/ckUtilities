// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.hpp"
#include "apple_gpu_statistics.hpp"
#include <IOKit/IOKitLib.h>
#include <cmath>
#import <Metal/Metal.h>

namespace ck::sysmon {
namespace {
CFNumberRef number(CFDictionaryRef values, CFStringRef key) {
    const auto value = CFDictionaryGetValue(values, key);
    return value && CFGetTypeID(value) == CFNumberGetTypeID() ? static_cast<CFNumberRef>(value) : nullptr;
}
struct Properties {
    CFMutableDictionaryRef value = nullptr;
    ~Properties() { if (value) CFRelease(value); }
};
}
void apply_apple_gpu_statistics(GpuSample& gpu, CFDictionaryRef properties,
                               std::optional<std::uint64_t> physical_memory) {
    gpu.memory_kind = GpuMemoryKind::UnifiedDriver;
    gpu.load.reset(); gpu.used.reset(); gpu.total.reset(); gpu.shared.reset();
    if (!properties) return;
    const auto raw = CFDictionaryGetValue(properties, CFSTR("PerformanceStatistics"));
    if (!raw || CFGetTypeID(raw) != CFDictionaryGetTypeID()) return;
    const auto stats = static_cast<CFDictionaryRef>(raw);
    if (const auto utilization = number(stats, CFSTR("Device Utilization %"))) {
        double value = 0;
        if (CFNumberGetValue(utilization, kCFNumberDoubleType, &value) && std::isfinite(value) && value >= 0 && value <= 100)
            gpu.load = value;
    }
    if (const auto memory = number(stats, CFSTR("In use system memory")); memory && !CFNumberIsFloatType(memory)) {
        std::int64_t value = 0;
        if (CFNumberGetValue(memory, kCFNumberSInt64Type, &value) && value >= 0 &&
            (!physical_memory || static_cast<std::uint64_t>(value) <= *physical_memory)) {
            gpu.used = static_cast<std::uint64_t>(value);
            if (physical_memory && *physical_memory) gpu.total = physical_memory;
        }
    }
}
void apple_gpus(Sample& sample) {
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        for (id<MTLDevice> device in devices) {
            if (sample.gpus.size() == 32) break;
            GpuSample gpu;
            gpu.id = "metal:" + std::to_string(device.registryID);
            gpu.name = device.name.UTF8String;
            if (device.hasUnifiedMemory) {
                const auto matching = IORegistryEntryIDMatching(device.registryID);
                const auto service = matching ? IOServiceGetMatchingService(kIOMainPortDefault, matching) : IO_OBJECT_NULL;
                Properties properties;
                bool read = false;
                if (service) {
                    read = IORegistryEntryCreateCFProperties(service, &properties.value, kCFAllocatorDefault, 0) == KERN_SUCCESS;
                    IOObjectRelease(service);
                }
                apply_apple_gpu_statistics(gpu, read ? properties.value : nullptr, sample.memory_total);
            }
            sample.gpus.push_back(std::move(gpu));
        }
        [devices release];
    }
}
} // namespace ck::sysmon
