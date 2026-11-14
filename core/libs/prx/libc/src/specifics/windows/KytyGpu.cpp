#include "prx/libc/include/KytyGpu.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#ifdef _WIN32
#include <windows.h>

namespace {

struct Kyty {
    std::uint64_t (*resolve)(const char* nid);
    void (*onMap)(std::uint64_t address, std::uint64_t bytes, std::uint32_t protection, void* section, std::uint64_t offset);
    void (*onUnmap)(std::uint64_t address, std::uint64_t bytes);
    void (*onProtect)(std::uint64_t address, std::uint64_t bytes, std::uint32_t protection);
    void (*onSectionClosed)(void* section);
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
        Load(module, path, "kyty_gpu_on_map", loaded.onMap);
        Load(module, path, "kyty_gpu_on_unmap", loaded.onUnmap);
        Load(module, path, "kyty_gpu_on_protect", loaded.onProtect);
        Load(module, path, "kyty_gpu_on_section_closed", loaded.onSectionClosed);
        return loaded;
    }();
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
    char nid[32] = {};
    const auto length = std::strcspn(name, "#");
    if (length >= sizeof(nid)) return target;
    std::memcpy(nid, name, length);
    if (const auto address = Dll().resolve(nid); address != 0) return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    std::fprintf(stderr, "[kyty-gpu] not in Kyty, bound to a stub that returns 0: %s %s\n", library, name);
    return reinterpret_cast<void*>(&MissingImport);
}

void KytyGpuOnMap(std::uintptr_t address, std::size_t bytes, std::uint32_t protection, void* section, std::uint64_t offset) {
    if (KytyGpuEnabled()) Dll().onMap(address, bytes, protection, section, offset);
}

void KytyGpuOnUnmap(std::uintptr_t address, std::size_t bytes) {
    if (KytyGpuEnabled()) Dll().onUnmap(address, bytes);
}

void KytyGpuOnProtect(std::uintptr_t address, std::size_t bytes, std::uint32_t protection) {
    if (KytyGpuEnabled()) Dll().onProtect(address, bytes, protection);
}

void KytyGpuOnSectionClosed(void* section) {
    if (KytyGpuEnabled()) Dll().onSectionClosed(section);
}

#else

void KytyGpuOnMap(std::uintptr_t, std::size_t, std::uint32_t, void*, std::uint64_t) {}
void KytyGpuOnUnmap(std::uintptr_t, std::size_t) {}
void KytyGpuOnProtect(std::uintptr_t, std::size_t, std::uint32_t) {}
void KytyGpuOnSectionClosed(void*) {}

bool KytyGpuEnabled() {
    return false;
}

void* KytyGpuImport(void* target, const char*, const char*) {
    return target;
}

#endif
