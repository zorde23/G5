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

template <std::size_t PageBytes>
class PageTable {
public:
    std::atomic<std::uint8_t>* Page(std::uintptr_t address, bool create) {
        const auto page = address / PageBytes;
        auto& slot = chunks[page / ChunkEntries];
        auto* chunk = slot.load(std::memory_order_acquire);
        if (chunk == nullptr && create) {
            auto fresh = std::unique_ptr<Chunk>(new (std::nothrow) Chunk{});
            if (fresh == nullptr) return nullptr;
            if (slot.compare_exchange_strong(chunk, fresh.get(), std::memory_order_acq_rel, std::memory_order_acquire)) chunk = fresh.release();
        }
        return chunk == nullptr ? nullptr : &chunk->pages[page % ChunkEntries];
    }

private:
    struct Chunk {
        std::array<std::atomic<std::uint8_t>, ChunkEntries> pages{};
    };
    std::array<std::atomic<Chunk*>, (DonationEnd / PageBytes + ChunkEntries - 1) / ChunkEntries> chunks{};
};

constinit PageTable<DonationPageBytes> g_donated;
constinit PageTable<HostPageBytes> g_hostPages;

bool InRange(std::uintptr_t address, std::size_t bytes) {
    return bytes != 0 && address < DonationEnd && bytes <= DonationEnd - address;
}

std::uintptr_t ClampedEnd(std::uintptr_t address, std::size_t bytes) {
    return bytes > DonationEnd - address ? DonationEnd : address + bytes;
}

}

bool GuestArenaDonate_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (!InRange(address, bytes)) return false;
    const auto first = address / DonationPageBytes;
    const auto last = (address + bytes - 1) / DonationPageBytes;
    for (auto page = first; page <= last; ++page) {
        if (g_donated.Page(page * DonationPageBytes, true) == nullptr) return false;
    }
    for (auto page = address / HostPageBytes; page <= (address + bytes - 1) / HostPageBytes; ++page) {
        auto* entry = g_hostPages.Page(page * HostPageBytes, true);
        if (entry == nullptr) return false;
        entry->store(0, std::memory_order_release);
    }
    for (auto page = first; page <= last; ++page) g_donated.Page(page * DonationPageBytes, false)->store(1, std::memory_order_release);
    return true;
}

void GuestArenaWithdraw_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (bytes == 0 || address >= DonationEnd) return;
    const auto end = ClampedEnd(address, bytes);
    for (auto page = address / DonationPageBytes; page <= (end - 1) / DonationPageBytes; ++page) {
        if (auto* entry = g_donated.Page(page * DonationPageBytes, false)) entry->store(0, std::memory_order_release);
    }
    for (auto page = address / HostPageBytes; page <= (end - 1) / HostPageBytes; ++page) {
        if (auto* entry = g_hostPages.Page(page * HostPageBytes, false)) entry->store(0, std::memory_order_release);
    }
}

bool GuestArenaDonatedAny_nid_no_patch(std::uintptr_t address, std::size_t bytes) {
    if (bytes == 0 || address >= DonationEnd) return false;
    const auto end = ClampedEnd(address, bytes);
    for (auto page = address / DonationPageBytes; page <= (end - 1) / DonationPageBytes; ++page) {
        if (GuestArenaDonated_nid_no_patch(page * DonationPageBytes)) return true;
    }
    return false;
}

bool GuestArenaDonated_nid_no_patch(std::uintptr_t address) {
    if (address >= DonationEnd) return false;
    const auto* entry = g_donated.Page(address, false);
    return entry != nullptr && entry->load(std::memory_order_acquire) != 0;
}

bool GuestArenaSetHostPageState_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::uint8_t mask, std::uint8_t value) {
    if (!InRange(address, bytes)) return false;
    for (auto page = address / HostPageBytes; page <= (address + bytes - 1) / HostPageBytes; ++page) {
        auto* entry = g_hostPages.Page(page * HostPageBytes, true);
        if (entry == nullptr) return false;
        auto current = entry->load(std::memory_order_relaxed);
        while (!entry->compare_exchange_weak(current, static_cast<std::uint8_t>((current & ~mask) | (value & mask)), std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    }
    return true;
}

std::uint8_t GuestArenaHostPageState_nid_no_patch(std::uintptr_t address) {
    if (address >= DonationEnd) return 0;
    const auto* entry = g_hostPages.Page(address, false);
    return entry == nullptr ? 0 : entry->load(std::memory_order_acquire);
}

}
