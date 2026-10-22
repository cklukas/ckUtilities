// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_monitor.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#endif

namespace ck::sysmon {
void native_gpus(Sample& sample);
Sample native_sample() {
    Sample sample;
    sample.reachable = true;
#if defined(_WIN32)
    sample.system = "Windows";
    FILETIME idle{}, kernel{}, user{};
    if (::GetSystemTimes(&idle, &kernel, &user)) {
        const auto ticks = [](FILETIME t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        sample.cpu = CpuTicks{ticks(kernel) + ticks(user), ticks(idle)};
    }
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        sample.memory_total = memory.ullTotalPhys;
        sample.memory_used = memory.ullTotalPhys - memory.ullAvailPhys;
    }
#else
    utsname system{};
    if (::uname(&system) == 0) sample.system = std::string(system.sysname) + " / " + system.machine;
#if defined(__APPLE__)
    host_cpu_load_info_data_t cpu{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    const auto host = ::mach_host_self();
    if (::host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count) == KERN_SUCCESS) {
        std::uint64_t total = 0;
        for (const auto tick : cpu.cpu_ticks) total += tick;
        sample.cpu = CpuTicks{total, cpu.cpu_ticks[CPU_STATE_IDLE]};
    }
    std::uint64_t total_memory = 0; std::size_t length = sizeof(total_memory);
    vm_statistics64_data_t vm{}; count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (::sysctlbyname("hw.memsize", &total_memory, &length, nullptr, 0) == 0 &&
        ::host_page_size(host, &page) == KERN_SUCCESS &&
        ::host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        // Speculative pages are already part of free_count (Mach vm_statistics.h).
        const auto available = std::min(total_memory, (static_cast<std::uint64_t>(vm.free_count) + vm.inactive_count) * page);
        sample.memory_total = total_memory; sample.memory_used = total_memory - available;
    }
    ::mach_port_deallocate(::mach_task_self(), host);
#elif defined(__linux__)
    std::ifstream stat("/proc/stat"); std::string name; stat >> name;
    std::uint64_t value = 0, total = 0, idle = 0;
    // Linux guest and guest_nice are already included in user and nice.
    if (name == "cpu") {
        std::string line; std::getline(stat, line); std::istringstream fields(line);
        int index = 0;
        while (index < 8 && fields >> value) { total += value; if (index == 3 || index == 4) idle += value; ++index; }
        if (index >= 4) sample.cpu = CpuTicks{total, idle};
    }
    std::ifstream mem("/proc/meminfo"); std::string key, unit, line;
    std::optional<std::uint64_t> total_bytes, available;
    while (std::getline(mem, line)) {
        std::istringstream fields(line);
        if (!(fields >> key >> value >> unit) || unit != "kB") continue;
        if (key == "MemTotal:") total_bytes = value * 1024;
        if (key == "MemAvailable:") available = value * 1024;
    }
    if (total_bytes && available && *available <= *total_bytes) {
        sample.memory_total = total_bytes; sample.memory_used = *total_bytes - *available;
    }
#endif
#endif
    if (const auto threads = std::thread::hardware_concurrency())
        sample.system += " / " + std::to_string(threads) + " threads";
    native_gpus(sample);
    add_ollama(sample);
    return sample;
}
} // namespace ck::sysmon
