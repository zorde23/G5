#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace GuestArena {
namespace {

constexpr std::uintptr_t DonationEnd = 0x000000FC00000000ull + GuestAddressOffset;
constexpr std::size_t ChunkEntries = 65536;
constexpr std::size_t ChunkCount = (DonationEnd / DonationPageBytes + ChunkEntries - 1) / ChunkEntries;

struct Chunk {
    std::array<std::atomic<std::uint8_t>, ChunkEntries> pages{};
};

constinit std::array<std::atomic<Chunk*>, ChunkCount> g_chunks{};

std::atomic<std::uint8_t>* Page(std::uintptr_t address, bool create) {
    const auto page = address / DonationPageBytes;
    auto& slot = g_chunks[page / ChunkEntries];
    auto* chunk = slot.load(std::memory_order_acquire);
    if (chunk == nullptr && create) {
        auto fresh = std::unique_ptr<Chunk>(new (std::nothrow) Chunk{});
        if (fresh == nullptr) return nullptr;
        if (slot.compare_exchange_strong(chunk, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) chunk = fresh.release();
    }
    return chunk == nullptr ? nullptr : &chunk->pages[page % ChunkEntries];
}

bool InRange(std::uintptr_t address, std::size_t bytes) {
    return bytes != 0 && address < DonationEnd && bytes <= DonationEnd - address;
}

}

bool GuestArenaDonate_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!InRange(address, bytes)) return false;
    const auto first = address / DonationPageBytes;
    const auto last = (address + bytes - 1) / DonationPageBytes;
    for (auto page = first; page <= last; ++page) {
        if (Page(page * DonationPageBytes, true) == nullptr) return false;
    }
    for (auto page = first; page <= last; ++page) Page(page * DonationPageBytes, false)->store(1, std::memory_order_release);
    return true;
}

void GuestArenaWithdraw_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (bytes == 0 || address >= DonationEnd) return;
    const auto end = bytes > DonationEnd - address ? DonationEnd : address + bytes;
    for (auto page = address / DonationPageBytes; page <= (end - 1) / DonationPageBytes; ++page) {
        if (auto* entry = Page(page * DonationPageBytes, false)) entry->store(0, std::memory_order_release);
    }
}

bool GuestArenaDonatedAny_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (bytes == 0 || address >= DonationEnd) return false;
    const auto end = bytes > DonationEnd - address ? DonationEnd : address + bytes;
    for (auto page = address / DonationPageBytes; page <= (end - 1) / DonationPageBytes; ++page) {
        if (GuestArenaDonated_nid_no_patch(page * DonationPageBytes)) return true;
    }
    return false;
}

bool GuestArenaDonated_nid_no_patch(std::uintptr_t address) {
    if (address >= DonationEnd) return false;
    const auto* entry = Page(address, false);
    return entry != nullptr && entry->load(std::memory_order_acquire) != 0;
}

}
