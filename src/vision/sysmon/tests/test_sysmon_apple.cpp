// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cvision/testing/cktest.hpp"
#include "apple_gpu_statistics.hpp"
#include <limits>

namespace {
struct Snapshot {
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef stats = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    Snapshot() { CFDictionarySetValue(properties, CFSTR("PerformanceStatistics"), stats); }
    ~Snapshot() { CFRelease(properties); CFRelease(stats); }
    void integer(CFStringRef key, std::int64_t value) {
        auto number = CFNumberCreate(nullptr, kCFNumberSInt64Type, &value);
        CFDictionarySetValue(stats, key, number); CFRelease(number);
    }
    void real(CFStringRef key, double value) {
        auto number = CFNumberCreate(nullptr, kCFNumberDoubleType, &value);
        CFDictionarySetValue(stats, key, number); CFRelease(number);
    }
};
}
CK_TEST(sysmon_apple_registry_statistics_keep_unified_accounting_distinct_from_allocations) {
    Snapshot snapshot;
    snapshot.real(CFSTR("Device Utilization %"), 47.5);
    snapshot.integer(CFSTR("In use system memory"), 8LL << 30);
    snapshot.integer(CFSTR("Alloc system memory"), 24LL << 30);
    ck::sysmon::GpuSample gpu;
    ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, 32ULL << 30);
    CK_CHECK(gpu.load == 47.5); CK_CHECK(gpu.used == (8ULL << 30)); CK_CHECK(gpu.total == (32ULL << 30));
    CK_CHECK(!gpu.shared); CK_CHECK(gpu.memory_kind == ck::sysmon::GpuMemoryKind::UnifiedDriver);
}
CK_TEST(sysmon_apple_registry_statistics_reject_missing_wrong_type_and_out_of_range_values) {
    Snapshot snapshot; ck::sysmon::GpuSample gpu;
    for (double invalid : {-1.0, 101.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        snapshot.real(CFSTR("Device Utilization %"), invalid);
        ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, 32ULL << 30); CK_CHECK(!gpu.load);
    }
    snapshot.integer(CFSTR("In use system memory"), -1);
    ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, 32ULL << 30); CK_CHECK(!gpu.used);
    snapshot.integer(CFSTR("In use system memory"), 33LL << 30);
    ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, 32ULL << 30); CK_CHECK(!gpu.used);
    snapshot.real(CFSTR("In use system memory"), 1.5);
    CFDictionarySetValue(snapshot.stats, CFSTR("Device Utilization %"), CFSTR("47"));
    ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, {}); CK_CHECK(!gpu.load); CK_CHECK(!gpu.used);
    CFDictionarySetValue(snapshot.properties, CFSTR("PerformanceStatistics"), kCFBooleanTrue);
    ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, {}); CK_CHECK(!gpu.load); CK_CHECK(!gpu.used);
}
CK_TEST(sysmon_apple_registry_zero_is_valid_and_missing_snapshots_clear_old_values) {
    Snapshot snapshot; ck::sysmon::GpuSample gpu;
    snapshot.integer(CFSTR("Device Utilization %"), 0); snapshot.integer(CFSTR("In use system memory"), 0);
    ck::sysmon::apply_apple_gpu_statistics(gpu, snapshot.properties, {});
    CK_CHECK(gpu.load == 0.0); CK_CHECK(gpu.used == 0); CK_CHECK(!gpu.total);
    ck::sysmon::apply_apple_gpu_statistics(gpu, nullptr, 32ULL << 30);
    CK_CHECK(!gpu.load); CK_CHECK(!gpu.used); CK_CHECK(!gpu.total);
}
