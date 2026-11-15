#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "prx/libc/include/GuestArena.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

constexpr std::size_t BlockBytes = 0x10000;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Counts {
    std::uint64_t maps = 0, unmaps = 0, unmapsAfterTheSystem = 0, protects = 0, sectionsClosed = 0, faults = 0, hostWrites = 0;
};

Counts FakeCounts() {
    Counts counts;
    const auto module = GetModuleHandleA("kyty_gpu_fake.dll");
    if (module == nullptr) return counts;
    const auto read = reinterpret_cast<void (*)(std::uint64_t*)>(reinterpret_cast<void*>(GetProcAddress(module, "kyty_fake_counts")));
    std::uint64_t values[7]{};
    read(values);
    return Counts{values[0], values[1], values[2], values[3], values[4], values[5], values[6]};
}

DWORD Protection(const void* pointer) {
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(pointer, &memory, sizeof(memory)) == sizeof(memory), "VirtualQuery failed");
    return memory.Protect;
}

bool KytyProtect(const void* pointer, std::uint64_t bytes, std::uint32_t tracker) {
    const auto protect = reinterpret_cast<bool (*)(std::uint64_t, std::uint64_t, std::uint32_t)>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kyty_gpu_fake.dll"), "kyty_fake_protect")));
    Require(protect != nullptr, "the fake Kyty GPU DLL has no kyty_fake_protect");
    return protect(reinterpret_cast<std::uintptr_t>(pointer), bytes, tracker);
}

void CheckEffectiveProtection(void* block) {
    auto* bytes = static_cast<std::byte*>(block);
    Require(KytyProtect(bytes, 0x1000, 1), "aps5_protect_host refused a donated page");
    Require(Protection(bytes) == PAGE_READONLY && Protection(bytes + 0x1000) == PAGE_READWRITE, "aps5_protect_host did not make exactly the tracked page read-only");
    GuestArena::GuestArenaSetProtection_nid_postfix(reinterpret_cast<std::uintptr_t>(block), BlockBytes, PAGE_READWRITE);
    DWORD previous = 0;
    Require(VirtualProtect(block, BlockBytes, PAGE_READWRITE, &previous) != 0, "cannot protect the block as libkernel's mprotect does");
    GuestArena::GuestArenaReapplyHostProtection_nid_no_patch(reinterpret_cast<std::uintptr_t>(block), BlockBytes);
    Require(Protection(bytes) == PAGE_READONLY && Protection(bytes + 0x1000) == PAGE_READWRITE, "a game mprotect lifted the tracker's protection instead of keeping the stricter one");
    Require(KytyProtect(bytes + 0x3000, 0x2000, 2), "aps5_protect_host refused a range across two 16 KiB views");
    Require(Protection(bytes + 0x3000) == PAGE_NOACCESS && Protection(bytes + 0x4000) == PAGE_NOACCESS, "a protection across two views did not reach both pages");
    Require(KytyProtect(bytes + 0x4000, 0x3000, 1), "aps5_protect_host refused three pages of one view");
    Require(Protection(bytes + 0x4000) == PAGE_READONLY && Protection(bytes + 0x5000) == PAGE_READONLY && Protection(bytes + 0x6000) == PAGE_READONLY && Protection(bytes + 0x7000) == PAGE_READWRITE, "a protection of three pages of one view did not reach exactly those pages");
    Require(KytyProtect(bytes, BlockBytes, 0), "aps5_protect_host refused to clear the tracker");
    Require(Protection(bytes) == PAGE_READWRITE && Protection(bytes + 0x4000) == PAGE_READWRITE, "clearing the tracker did not restore the game's protection");
    Require(!KytyProtect(bytes + BlockBytes, 0x1000, 1), "aps5_protect_host accepted a page that is not donated");
    const auto resolve = reinterpret_cast<std::uint64_t (*)(const char*)>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kyty_gpu_fake.dll"), "kyty_fake_resolve")));
    Require(resolve != nullptr && resolve("no such export") == 0, "the host resolver answered a name no library exports");
}

void MapDirect(void* block, HANDLE section) {
    GuestArena::GuestArenaMapDirect_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
}

void Check(bool kyty) {
    auto* block = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, BlockBytes, nullptr);
    Require(section != nullptr, "cannot create a section");
    MapDirect(block, section);
    std::uintptr_t start = 0, end = 0;
    const bool resident = GuestArena::GuestArenaResidentSpan_nid_postfix(address, BlockBytes, &start, &end);
    Require(resident != kyty, kyty ? "a direct mapping with the Kyty GPU is a resident run" : "a direct mapping without the Kyty GPU is not resident");
    for (std::size_t offset = 0; offset < BlockBytes; offset += GuestArena::DonationPageBytes) {
        Require(GuestArena::GuestArenaDonated_nid_no_patch(address + offset) == kyty, kyty ? "a page of the direct mapping is not donated" : "a direct mapping is donated without the Kyty GPU");
    }
    *static_cast<volatile std::uint32_t*>(block) = 0x5a5a5a5a;
    Require(*static_cast<volatile std::uint32_t*>(block) == 0x5a5a5a5a, "the direct mapping does not hold a store");
    if (kyty) {
        DWORD previous = 0;
        Require(VirtualProtect(block, 0x1000, PAGE_READONLY, &previous) != 0, "cannot write-protect a donated page as the Kyty tracker does");
        *static_cast<volatile std::uint32_t*>(block) = 0x6b6b6b6b;
        Require(*static_cast<volatile std::uint32_t*>(block) == 0x6b6b6b6b && FakeCounts().faults == 1, "a write to a page Kyty protects did not reach Kyty's fault handler once and resume");
        Require(VirtualProtect(block, 0x1000, PAGE_READONLY, &previous) != 0, "cannot write-protect a donated page again");
        Require(GuestArena::GuestArenaBeginHostWrite_nid_postfix(block, 16), "a host write to a donated page was refused");
        const std::uint32_t source = 0x7c7c7c7c;
        std::memcpy(block, &source, sizeof(source));
        GuestArena::GuestArenaEndHostWrite_nid_postfix(block, 16);
        const auto afterHostWrite = FakeCounts();
        Require(afterHostWrite.hostWrites == 1 && afterHostWrite.faults == 1 && *static_cast<volatile std::uint32_t*>(block) == 0x7c7c7c7c, "a host write to a page Kyty protects was not prepared by Kyty before it landed");
        CheckEffectiveProtection(block);
    }
    GuestArena::GuestArenaSetProtection_nid_postfix(address, BlockBytes, PAGE_READONLY);
    GuestArena::GuestArenaSetProtection_nid_postfix(address, BlockBytes, PAGE_READWRITE);
    GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
    Require(GuestArena::GuestArenaDonated_nid_no_patch(address) == kyty, "a commit over a direct mapping (the mprotect path) changed its donation");
    MapDirect(block, section);
    Require(GuestArena::GuestArenaDonated_nid_no_patch(address) == kyty, "a direct mapping over a direct mapping lost its donation");
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(address), "a reset direct mapping is still donated");
    const auto counts = FakeCounts();
    if (kyty) {
        Require(counts.maps == 2, "Kyty was not told about each of the 2 direct mappings");
        Require(counts.unmaps == 2, "Kyty was not told about each of the 2 times a donated range left (mapped over, reset)");
        Require(counts.unmapsAfterTheSystem == 0, "Kyty was told about an unmap after the system had unmapped the range");
        Require(counts.protects == 4, "Kyty was not told about each protection change of the donated range (3 protects and the commit)");
    } else {
        Require(GetModuleHandleA("kyty_gpu_fake.dll") == nullptr, "the Kyty GPU DLL was loaded without APS5_KYTY_GPU");
    }
    CloseHandle(section);
}

}

int main(int argc, char** argv) {
    const bool kyty = argc > 1 && std::strcmp(argv[1], "kyty") == 0;
    try {
        Check(kyty);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "guest donation map (%s): %s\n", kyty ? "kyty" : "g5", error.what());
        return 1;
    }
    std::printf("guest donation map (%s): ok\n", kyty ? "kyty" : "g5");
    return 0;
}
