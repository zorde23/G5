#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

std::atomic<std::uint64_t> g_maps{0};
std::atomic<std::uint64_t> g_unmaps{0};
std::atomic<std::uint64_t> g_unmapsAfterTheSystem{0};
std::atomic<std::uint64_t> g_protects{0};
std::atomic<std::uint64_t> g_sectionsClosed{0};

}

extern "C" std::uint64_t kyty_gpu_resolve(const char* nid) {
    return nid != nullptr && std::strcmp(nid, "kytyHasThis") == 0 ? 0x1234500 : 0;
}

extern "C" void kyty_gpu_on_map(std::uint64_t, std::uint64_t, std::uint32_t, void*, std::uint64_t) {
    g_maps.fetch_add(1);
}

extern "C" void kyty_gpu_on_unmap(std::uint64_t address, std::uint64_t) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) g_unmapsAfterTheSystem.fetch_add(1);
    g_unmaps.fetch_add(1);
}

extern "C" void kyty_gpu_on_protect(std::uint64_t, std::uint64_t, std::uint32_t) {
    g_protects.fetch_add(1);
}

extern "C" void kyty_gpu_on_section_closed(void*) {
    g_sectionsClosed.fetch_add(1);
}

extern "C" void kyty_fake_counts(std::uint64_t* counts) {
    counts[0] = g_maps.load();
    counts[1] = g_unmaps.load();
    counts[2] = g_unmapsAfterTheSystem.load();
    counts[3] = g_protects.load();
    counts[4] = g_sectionsClosed.load();
}
