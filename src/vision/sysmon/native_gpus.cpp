// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_monitor.hpp"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <d3dkmthk.h>
#include <cfgmgr32.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ck::sysmon {
#if !defined(__APPLE__)
namespace {
// ABI declarations from NVIDIA's published NVML reference. No SDK or driver
// dependency at link time. Every optional query is checked independently.
class Nvml {
public:
    Nvml() {
#if defined(_WIN32)
        library_ = ::LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
        library_ = ::dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        const auto init = symbol<int(*)()>("nvmlInit_v2");
        active_ = init && init() == 0;
    }
    ~Nvml() {
        const auto shutdown = symbol<int(*)()>("nvmlShutdown");
        if (active_ && shutdown) shutdown();
#if defined(_WIN32)
        if (library_) ::FreeLibrary(library_);
#else
        if (library_) ::dlclose(library_);
#endif
    }
    template<class F> F symbol(const char* name) {
        if (!library_) return nullptr;
#if defined(_WIN32)
        const auto pointer = ::GetProcAddress(library_, name);
#else
        const auto pointer = ::dlsym(library_, name);
#endif
        F function{};
        static_assert(sizeof(function) == sizeof(pointer));
        std::memcpy(&function, &pointer, sizeof(function));
        return function;
    }
    void collect(Sample& sample, [[maybe_unused]] const std::map<std::string, std::string>& pci_ids) {
        if (!active_) return;
        const auto get_count = symbol<int(*)(unsigned*)>("nvmlDeviceGetCount_v2");
        const auto get_device = symbol<int(*)(unsigned, void**)>("nvmlDeviceGetHandleByIndex_v2");
        const auto get_name = symbol<int(*)(void*, char*, unsigned)>("nvmlDeviceGetName");
        const auto get_uuid = symbol<int(*)(void*, char*, unsigned)>("nvmlDeviceGetUUID");
        struct Utilization { unsigned gpu, memory; };
        struct Memory { unsigned long long total, free, used; };
        const auto get_util = symbol<int(*)(void*, Utilization*)>("nvmlDeviceGetUtilizationRates");
        const auto get_memory = symbol<int(*)(void*, Memory*)>("nvmlDeviceGetMemoryInfo");
        const auto get_temp = symbol<int(*)(void*, unsigned, unsigned*)>("nvmlDeviceGetTemperature");
        const auto get_power = symbol<int(*)(void*, unsigned*)>("nvmlDeviceGetPowerUsage");
        unsigned count = 0;
        if (!get_count || !get_device || get_count(&count) != 0) return;
        for (unsigned index = 0; index < std::min(count, 32u); ++index) {
            void* device = nullptr;
            if (get_device(index, &device) != 0) continue;
            GpuSample gpu;
            char name[256]{}, uuid[96]{};
            gpu.name = get_name && get_name(device, name, sizeof(name)) == 0 ? name : "NVIDIA GPU";
            gpu.id = get_uuid && get_uuid(device, uuid, sizeof(uuid)) == 0 ? uuid : "nvidia:" + std::to_string(index);
#if defined(_WIN32)
            // Resolve NVML PCI address to a DXGI LUID through the documented
            // D3DKMT adapter address; identical card models never join by name.
            struct PciInfo { char legacy[16]; unsigned domain, bus, device, device_id, subsystem; char bus_id[32]; };
            const auto get_pci = symbol<int(*)(void*, PciInfo*)>("nvmlDeviceGetPciInfo_v3");
            PciInfo pci{};
            if (get_pci && get_pci(device, &pci) == 0) {
                const auto key = "pci:" + std::to_string(pci.bus) + ":" + std::to_string(pci.device);
                if (pci_ids.contains(key)) gpu.id = pci_ids.at(key);
            }
#endif
            Utilization utilization{};
            if (get_util && get_util(device, &utilization) == 0 && utilization.gpu <= 100) gpu.load = utilization.gpu;
            Memory memory{};
            if (get_memory && get_memory(device, &memory) == 0 && memory.total && memory.used <= memory.total) {
                gpu.used = memory.used; gpu.total = memory.total;
            }
            unsigned temperature = 0, power = 0;
            if (get_temp && get_temp(device, 0, &temperature) == 0) gpu.temperature = temperature;
            if (get_power && get_power(device, &power) == 0) gpu.watts = power / 1000.0;
            auto existing = std::find_if(sample.gpus.begin(), sample.gpus.end(), [&](const auto& g) { return g.id == gpu.id; });
            if (existing == sample.gpus.end()) sample.gpus.push_back(std::move(gpu));
            else {
                existing->name = gpu.name;
                if (gpu.total) existing->total = gpu.total;
                if (gpu.load) existing->load = gpu.load;
                if (gpu.temperature) existing->temperature = gpu.temperature;
                if (gpu.watts) existing->watts = gpu.watts;
                // PDH owns WDDM memory accounting; NVML's process/device view
                // can omit system-managed allocations on Windows.
            }
        }
    }
private:
#if defined(_WIN32)
    HMODULE library_ = nullptr;
#else
    void* library_ = nullptr;
#endif
    bool active_ = false;
};
#if defined(_WIN32)
std::string utf8_name(const wchar_t* name) {
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "GPU";
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), size, nullptr, nullptr);
    result.pop_back(); return result;
}
std::map<std::string, std::string> windows_gpus(Sample& sample) {
    std::map<std::string, std::string> pci_ids;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return pci_ids;
    for (UINT index = 0; index < 32; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::ostringstream id; id << "luid_0x" << std::hex; id.width(8); id.fill('0');
            id << static_cast<unsigned>(desc.AdapterLuid.HighPart) << "_0x"; id.width(8); id << desc.AdapterLuid.LowPart;
            D3DKMT_OPENADAPTERFROMLUID opened{}; opened.AdapterLuid = desc.AdapterLuid;
            const bool opened_ok = ::D3DKMTOpenAdapterFromLuid(&opened) == 0;
            D3DKMT_PHYSICAL_ADAPTER_COUNT physical_count{1};
            if (opened_ok) {
                D3DKMT_QUERYADAPTERINFO info{}; info.hAdapter = opened.hAdapter;
                info.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
                info.pPrivateDriverData = &physical_count; info.PrivateDriverDataSize = sizeof(physical_count);
                if (::D3DKMTQueryAdapterInfo(&info) != 0) physical_count.Count = 1;
            }
            for (UINT physical = 0; physical < std::min(physical_count.Count, 32u) && sample.gpus.size() < 32; ++physical) {
                GpuSample gpu; gpu.id = id.str() + "_phys_" + std::to_string(physical);
                gpu.name = utf8_name(desc.Description);
                if (physical_count.Count == 1 && desc.DedicatedVideoMemory) gpu.total = desc.DedicatedVideoMemory;
                if (opened_ok) {
                    // A linked display adapter may contain several physical
                    // cards with one LUID. Resolve each physical PnP instance
                    // to its PCI address through Configuration Manager.
                    wchar_t key[2048]{}; UINT length = 2048;
                    D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY pnp{};
                    pnp.PhysicalAdapterIndex = physical; pnp.PnPKeyType = D3DKMT_PNP_KEY_HARDWARE;
                    pnp.pDest = key; pnp.pCchDest = &length;
                    D3DKMT_QUERYADAPTERINFO info{}; info.hAdapter = opened.hAdapter;
                    info.Type = KMTQAITYPE_PHYSICALADAPTERPNPKEY;
                    info.pPrivateDriverData = &pnp; info.PrivateDriverDataSize = sizeof(pnp);
                    if (::D3DKMTQueryAdapterInfo(&info) == 0) {
                        std::wstring path(key), lower(path);
                        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                        const auto begin = lower.find(L"\\enum\\"), end = lower.rfind(L"\\device parameters");
                        if (begin != lower.npos && end != lower.npos && end > begin + 6) {
                            auto instance = path.substr(begin + 6, end - begin - 6);
                            DEVINST node = 0;
                            if (::CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
                                ULONG bus = 0, address = 0, type = 0, bytes = sizeof(bus);
                                const auto bus_status = ::CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_BUSNUMBER, &type, &bus, &bytes, 0);
                                bytes = sizeof(address);
                                const auto address_status = ::CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_ADDRESS, &type, &address, &bytes, 0);
                                if (bus_status == CR_SUCCESS && address_status == CR_SUCCESS)
                                    pci_ids["pci:" + std::to_string(bus) + ":" + std::to_string(address >> 16)] = gpu.id;
                            }
                        }
                    }
                    // Older drivers may not offer the physical PnP query.
                    // ADAPTERADDRESS is unambiguous only for an unlinked GPU.
                    if (physical_count.Count == 1) {
                        D3DKMT_ADAPTERADDRESS address{};
                        info.Type = KMTQAITYPE_ADAPTERADDRESS; info.pPrivateDriverData = &address; info.PrivateDriverDataSize = sizeof(address);
                        if (::D3DKMTQueryAdapterInfo(&info) == 0)
                            pci_ids["pci:" + std::to_string(address.BusNumber) + ":" + std::to_string(address.DeviceNumber)] = gpu.id;
                    }
                }
                sample.gpus.push_back(std::move(gpu));
            }
            if (opened_ok) { D3DKMT_CLOSEADAPTER closed{}; closed.hAdapter = opened.hAdapter; ::D3DKMTCloseAdapter(&closed); }
        }
        adapter->Release();
    }
    factory->Release();
    PDH_HQUERY query = nullptr;
    if (::PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return pci_ids;
    PDH_HCOUNTER load = nullptr, dedicated = nullptr, shared = nullptr;
    ::PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &load);
    ::PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &dedicated);
    ::PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &shared);
    ::PdhCollectQueryData(query);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (::PdhCollectQueryData(query) == ERROR_SUCCESS) {
        for (const auto counter : {load, dedicated, shared}) {
            if (!counter) continue;
            DWORD size = 0, count = 0;
            const DWORD format = counter == load ? PDH_FMT_DOUBLE | PDH_FMT_NOCAP100 : PDH_FMT_LARGE;
            if (::PdhGetFormattedCounterArrayW(counter, format, &size, &count, nullptr) != PDH_MORE_DATA || size > 16 * 1024 * 1024) continue;
            std::vector<std::uint64_t> storage((size + 7) / 8);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
            if (::PdhGetFormattedCounterArrayW(counter, format, &size, &count, items) != ERROR_SUCCESS) continue;
            for (auto& gpu : sample.gpus) {
                std::map<std::wstring, double> engines;
                std::uint64_t bytes = 0; bool reported = false;

                for (DWORD i = 0; i < count; ++i) {
                    if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                    const std::wstring name(items[i].szName ? items[i].szName : L"");
                    const std::string instance = utf8_name(name.c_str());
                    const auto identity = counter_gpu_id(instance);
                    if (!identity || *identity != gpu.id) continue;
                    const auto begin = instance.find("luid_");
                    reported = true;
                    if (counter == load) engines[std::wstring(name.substr(begin))] += items[i].FmtValue.doubleValue;
                    else if (items[i].FmtValue.largeValue >= 0) bytes += static_cast<std::uint64_t>(items[i].FmtValue.largeValue);
                }
                if (reported && counter == dedicated) gpu.used = bytes;
                if (reported && counter == shared) gpu.shared = bytes;
                if (reported && counter == load) {
                    double peak = 0; for (const auto& engine : engines) peak = std::max(peak, engine.second);
                    gpu.load = std::clamp(peak, 0.0, 100.0);
                }
            }
        }
    }
    ::PdhCloseQuery(query);
    return pci_ids;
}
#endif
}
#endif
#if defined(__APPLE__)
void apple_gpus(Sample& sample);
#endif
void native_gpus(Sample& sample) {
#if defined(__APPLE__)
    apple_gpus(sample);
#else
#if defined(_WIN32)
    const auto pci_ids = windows_gpus(sample);
#endif
#if !defined(_WIN32)
    const std::map<std::string, std::string> pci_ids;
#endif
    Nvml nvml; nvml.collect(sample, pci_ids);
#if defined(__linux__)
    linux_drm_gpus(sample, "/sys/class/drm", "/sys/kernel/debug/dri");
#endif
#endif
}
} // namespace ck::sysmon
