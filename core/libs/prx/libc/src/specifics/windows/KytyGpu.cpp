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

using Resolve = std::uint64_t (*)(const char* nid);

bool Routed(std::string_view library) {
    if (library.ends_with(".prx")) library.remove_suffix(4);
    return library == "libSceAgc" || library == "libSceAgcDriver" || library == "libSceVideoOut" || library == "libScePad";
}

Resolve Resolver() {
    static const Resolve resolve = [] {
        const char* path = std::getenv("APS5_KYTY_GPU_DLL");
        if (path == nullptr || *path == '\0') path = "kyty_gpu.dll";
        HMODULE module = LoadLibraryA(path);
        const auto function = module != nullptr ? GetProcAddress(module, "kyty_gpu_resolve") : nullptr;
        if (function == nullptr) {
            std::fprintf(stderr, "[kyty-gpu] FATAL: APS5_KYTY_GPU=1 but %s or its kyty_gpu_resolve cannot be loaded (error %lu)\n", path, GetLastError());
            std::fflush(stderr);
            std::abort();
        }
        return reinterpret_cast<Resolve>(reinterpret_cast<void*>(function));
    }();
    return resolve;
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
    if (const auto address = Resolver()(nid); address != 0) return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    std::fprintf(stderr, "[kyty-gpu] not in Kyty, bound to a stub that returns 0: %s %s\n", library, name);
    return reinterpret_cast<void*>(&MissingImport);
}

#else

bool KytyGpuEnabled() {
    return false;
}

void* KytyGpuImport(void* target, const char*, const char*) {
    return target;
}

#endif
