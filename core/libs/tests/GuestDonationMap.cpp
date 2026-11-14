#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "prx/libc/include/GuestArena.hpp"
#include <windows.h>
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

void Check(bool kyty) {
    auto* block = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, BlockBytes, nullptr);
    Require(section != nullptr, "cannot create a section");
    GuestArena::GuestArenaMapDirect_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
    std::uintptr_t start = 0, end = 0;
    const bool resident = GuestArena::GuestArenaResidentSpan_nid_postfix(address, BlockBytes, &start, &end);
    Require(resident != kyty, kyty ? "a direct mapping with the Kyty GPU is a resident run" : "a direct mapping without the Kyty GPU is not resident");
    for (std::size_t offset = 0; offset < BlockBytes; offset += GuestArena::DonationPageBytes) {
        Require(GuestArena::GuestArenaDonated_nid_no_patch(address + offset) == kyty, kyty ? "a page of the direct mapping is not donated" : "a direct mapping is donated without the Kyty GPU");
    }
    *static_cast<volatile std::uint32_t*>(block) = 0x5a5a5a5a;
    Require(*static_cast<volatile std::uint32_t*>(block) == 0x5a5a5a5a, "the direct mapping does not hold a store");
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(address), "a reset direct mapping is still donated");
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
