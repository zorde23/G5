#include "prx/libc/include/KytyGpu.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/WindowsMappings.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#ifdef _WIN32
#include <windows.h>

namespace {

std::atomic<std::uint64_t> g_maps{0}, g_mapBytes{0}, g_unmaps{0}, g_protects{0}, g_trackedPages[3]{}, g_kytyFaults{0};

struct Aps5Host {
    std::uint32_t version;
    bool (*protectHost)(std::uint64_t address, std::uint64_t bytes, std::uint32_t tracker);
    std::uint64_t (*resolve)(const char* nid);
};

std::uint64_t ResolveHostExport(const char* nid) {
    for (const char* library : {"libkernel.prx", "libc.prx", "libScePad.prx"}) {
        if (const auto module = GetModuleHandleA(library)) {
            if (const auto address = GetProcAddress(module, nid)) return reinterpret_cast<std::uint64_t>(address);
        }
    }
    return 0;
}

struct Kyty {
    std::uint64_t (*resolve)(const char* nid);
    void (*init)(const Aps5Host* host);
    void (*onMap)(std::uint64_t address, std::uint64_t bytes, std::uint32_t protection, void* section, std::uint64_t offset);
    void (*onUnmap)(std::uint64_t address, std::uint64_t bytes);
    void (*onProtect)(std::uint64_t address, std::uint64_t bytes, std::uint32_t protection);
    void (*onSectionClosed)(void* section);
    bool (*handleFault)(std::uint64_t address, std::uint32_t access, std::uint64_t rip, void* context);
    void (*prepareHostWrite)(std::uint64_t address, std::uint64_t bytes);
};

bool Routed(std::string_view library) {
    if (library.ends_with(".prx")) library.remove_suffix(4);
    return library == "libSceAgc" || library == "libSceAgcDriver" || library == "libSceVideoOut" || library == "libScePad";
}

template <typename Function>
void Load(HMODULE module, const char* path, const char* name, Function& function) {
    const auto address = module != nullptr ? GetProcAddress(module, name) : nullptr;
    if (address == nullptr) {
        std::fprintf(stderr, "[kyty-gpu] FATAL: APS5_KYTY_GPU=1 but %s or its %s cannot be loaded (error %lu)\n", path, name, GetLastError());
        std::fflush(stderr);
        std::abort();
    }
    function = reinterpret_cast<Function>(reinterpret_cast<void*>(address));
}

const Kyty& Dll() {
    static const Kyty kyty = [] {
        const char* path = std::getenv("APS5_KYTY_GPU_DLL");
        if (path == nullptr || *path == '\0') path = "kyty_gpu.dll";
        HMODULE module = LoadLibraryA(path);
        Kyty loaded{};
        Load(module, path, "kyty_gpu_resolve", loaded.resolve);
        Load(module, path, "kyty_gpu_init", loaded.init);
        Load(module, path, "kyty_gpu_on_map", loaded.onMap);
        Load(module, path, "kyty_gpu_on_unmap", loaded.onUnmap);
        Load(module, path, "kyty_gpu_on_protect", loaded.onProtect);
        Load(module, path, "kyty_gpu_on_section_closed", loaded.onSectionClosed);
        Load(module, path, "kyty_gpu_handle_fault", loaded.handleFault);
        Load(module, path, "kyty_gpu_prepare_host_write", loaded.prepareHostWrite);
        return loaded;
    }();
    return kyty;
}

std::atomic<bool> g_initStarted{false};

void InitOnce() {
    if (g_initStarted.exchange(true, std::memory_order_acq_rel)) return;
    static const Aps5Host host{2, &KytyGpuProtectHost, &ResolveHostExport};
    Dll().init(&host);
}

const Kyty& Ready() {
    const auto& kyty = Dll();
    InitOnce();
    return kyty;
}

std::atomic<bool> g_missingCalled{false};

std::uint64_t APS5_VABI MissingImport() {
    if (!g_missingCalled.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "[kyty-gpu] a NID that Kyty does not have was called; it returns 0 (the NIDs are listed at startup)\n");
    return 0;
}

}

bool KytyGpuEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_KYTY_GPU");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void* KytyGpuImport(void* target, const char* name, const char* library) {
    if (!KytyGpuEnabled() || name == nullptr || library == nullptr || !Routed(library)) return target;
    InitOnce();
    char nid[32] = {};
    const auto length = std::strcspn(name, "#");
    if (length >= sizeof(nid)) return target;
    std::memcpy(nid, name, length);
    if (const auto address = Dll().resolve(nid); address != 0) return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    std::fprintf(stderr, "[kyty-gpu] not in Kyty, bound to a stub that returns 0: %s %s\n", library, name);
    return reinterpret_cast<void*>(&MissingImport);
}

void KytyGpuOnMap(std::uintptr_t address, std::size_t bytes, std::uint32_t protection, void* section, std::uint64_t offset) {
    if (!KytyGpuEnabled()) return;
    g_maps.fetch_add(1, std::memory_order_relaxed);
    g_mapBytes.fetch_add(bytes, std::memory_order_relaxed);
    Ready().onMap(address, bytes, protection, section, offset);
}

void KytyGpuOnUnmap(std::uintptr_t address, std::size_t bytes) {
    if (!KytyGpuEnabled()) return;
    g_unmaps.fetch_add(1, std::memory_order_relaxed);
    Ready().onUnmap(address, bytes);
}

void KytyGpuOnProtect(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    if (!KytyGpuEnabled()) return;
    g_protects.fetch_add(1, std::memory_order_relaxed);
    Ready().onProtect(address, bytes, protection);
}

void KytyGpuOnSectionClosed(void* section) {
    if (KytyGpuEnabled()) Ready().onSectionClosed(section);
}

namespace {

constexpr std::uint8_t TrackerMask = 0x03;
constexpr std::uint8_t GameShift = 2;
constexpr std::uint8_t GameMask = 0x1c;
constexpr DWORD GameProtections[] = {PAGE_READWRITE, PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE};

std::uint8_t GameClass(std::uint32_t protection) {
    for (std::uint8_t index = 1; index < std::size(GameProtections); ++index) {
        if (GameProtections[index] == (protection & 0xff)) return index;
    }
    return 0;
}

DWORD Effective(std::uint8_t state) {
    const auto game = GameProtections[std::min<std::size_t>((state & GameMask) >> GameShift, std::size(GameProtections) - 1)];
    switch (state & TrackerMask) {
    case 1:
        if (game == PAGE_READWRITE) return PAGE_READONLY;
        if (game == PAGE_EXECUTE_READWRITE) return PAGE_EXECUTE_READ;
        return game;
    case 2: return PAGE_NOACCESS;
    default: return game;
    }
}

bool ApplyEffective(std::uintptr_t address, std::size_t bytes, bool trackedOnly) {
    bool applied = true;
    for (auto page = address & ~static_cast<std::uintptr_t>(GuestArena::HostPageBytes - 1); page < address + bytes; page += GuestArena::HostPageBytes) {
        for (auto state = GuestArena::GuestArenaHostPageState_nid_no_patch(page);;) {
            if (trackedOnly && (state & TrackerMask) == 0) break;
            DWORD previous = 0;
            GuestArena::WindowsMappings::RecordProtection(page, GuestArena::HostPageBytes, Effective(state), "protect a page the Kyty GPU tracks");
            applied = VirtualProtect(reinterpret_cast<void*>(page), GuestArena::HostPageBytes, Effective(state), &previous) != 0 && applied;
            const auto now = GuestArena::GuestArenaHostPageState_nid_no_patch(page);
            if (now == state) break;
            state = now;
        }
    }
    return applied;
}

}

void KytyGpuNoteGameProtection(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    if (!KytyGpuEnabled() || !GuestArena::GuestArenaDonatedAny_nid_no_patch(address, bytes)) return;
    GuestArena::GuestArenaSetHostPageState_nid_no_patch(address, bytes, GameMask, static_cast<std::uint8_t>(GameClass(protection) << GameShift));
    ApplyEffective(address, bytes, true);
}

void KytyGpuReapplyProtection(std::uintptr_t address, std::size_t bytes) {
    if (KytyGpuEnabled() && GuestArena::GuestArenaDonatedAny_nid_no_patch(address, bytes)) ApplyEffective(address, bytes, true);
}

bool KytyGpuProtectHost(std::uint64_t address, std::uint64_t bytes, std::uint32_t tracker) {
    if (tracker > 2 || bytes == 0 || address + bytes < address) return false;
    for (auto page = address & ~std::uint64_t{GuestArena::HostPageBytes - 1}; page < address + bytes; page += GuestArena::HostPageBytes) {
        if (!GuestArena::GuestArenaDonated_nid_no_patch(page)) return false;
    }
    g_trackedPages[tracker].fetch_add((address + bytes - (address & ~std::uint64_t{GuestArena::HostPageBytes - 1}) + GuestArena::HostPageBytes - 1) / GuestArena::HostPageBytes, std::memory_order_relaxed);
    return GuestArena::GuestArenaSetHostPageState_nid_no_patch(address, bytes, TrackerMask, static_cast<std::uint8_t>(tracker)) && ApplyEffective(address, bytes, false);
}

void KytyGpuPrepareHostWrite(std::uintptr_t address, std::size_t bytes) {
    if (KytyGpuEnabled() && GuestArena::GuestArenaDonatedAny_nid_no_patch(address, bytes)) Ready().prepareHostWrite(address, bytes);
}

bool KytyGpuHandleFault(std::uintptr_t address, std::uint32_t access, std::uint64_t rip, void* context) {
    if (!KytyGpuEnabled() || !GuestArena::GuestArenaDonated_nid_no_patch(address) || !Dll().handleFault(address, access, rip, context)) return false;
    g_kytyFaults.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void KytyGpuReportCounters() {
    if (!KytyGpuEnabled()) return;
    const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
    const auto maps = take(g_maps);
    const auto mib = take(g_mapBytes) >> 20u;
    const auto unmaps = take(g_unmaps);
    const auto protects = take(g_protects);
    const auto released = take(g_trackedPages[0]);
    const auto readOnly = take(g_trackedPages[1]);
    const auto noAccess = take(g_trackedPages[2]);
    std::fprintf(stderr, "[hib-mem] 10 s: on_map %llu (%llu MiB), on_unmap %llu, on_protect %llu; host pages the Kyty GPU protected %llu read-only, %llu no-access, %llu released; faults it handled %llu\n", maps, mib, unmaps, protects, readOnly, noAccess, released, take(g_kytyFaults));
}

#else

void KytyGpuOnMap(std::uintptr_t, std::size_t, std::uint32_t, void*, std::uint64_t) {}
void KytyGpuOnUnmap(std::uintptr_t, std::size_t) {}
void KytyGpuOnProtect(std::uintptr_t, std::size_t, std::uint32_t) {}
void KytyGpuOnSectionClosed(void*) {}
void KytyGpuPrepareHostWrite(std::uintptr_t, std::size_t) {}
void KytyGpuNoteGameProtection(std::uintptr_t, std::size_t, std::uint32_t) {}
void KytyGpuReapplyProtection(std::uintptr_t, std::size_t) {}
bool KytyGpuProtectHost(std::uint64_t, std::uint64_t, std::uint32_t) {
    return false;
}
void KytyGpuReportCounters() {}

bool KytyGpuHandleFault(std::uintptr_t, std::uint32_t, std::uint64_t, void*) {
    return false;
}

bool KytyGpuEnabled() {
    return false;
}

void* KytyGpuImport(void* target, const char*, const char*) {
    return target;
}

#endif
