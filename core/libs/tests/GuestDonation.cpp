#include "prx/libc/include/GuestArena.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uintptr_t Base = 0x0000000900000000ull + GuestArena::GuestAddressOffset;
constexpr std::size_t Page = GuestArena::DonationPageBytes;

void SinglePages() {
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(Base), "a page counts as donated before any donation");
    Require(GuestArena::GuestArenaDonate_nid_no_patch(Base + Page + 1, Page), "a donation inside the arena was refused");
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(Base), "the page before the donation counts as donated");
    Require(GuestArena::GuestArenaDonated_nid_no_patch(Base + Page) && GuestArena::GuestArenaDonated_nid_no_patch(Base + 2 * Page + Page - 1), "an unaligned donation did not cover both pages it touches");
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(Base + 3 * Page), "the page after the donation counts as donated");
    GuestArena::GuestArenaWithdraw_nid_no_patch(Base + Page, 2 * Page);
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(Base + Page) && !GuestArena::GuestArenaDonated_nid_no_patch(Base + 2 * Page), "a withdrawn range still counts as donated");
    Require(!GuestArena::GuestArenaDonate_nid_no_patch(0x000000FC00000000ull + GuestArena::GuestAddressOffset, Page), "a donation past the application area was accepted");
    Require(!GuestArena::GuestArenaDonate_nid_no_patch(Base, 0), "an empty donation was accepted");
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(~std::uintptr_t{0}), "the top of the address space counts as donated");
}

void ConcurrentReader() {
    constexpr std::uintptr_t start = Base + 0x40000000ull;
    constexpr std::size_t pages = 4096;
    std::atomic<bool> done{false};
    std::atomic<std::size_t> seen{0};
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            std::size_t count = 0;
            for (std::size_t page = 0; page < pages; ++page) count += GuestArena::GuestArenaDonated_nid_no_patch(start + page * Page) ? 1 : 0;
            seen.store(count, std::memory_order_relaxed);
        }
    });
    for (std::size_t page = 0; page < pages; ++page) Require(GuestArena::GuestArenaDonate_nid_no_patch(start + page * Page, Page), "a donation was refused while a reader asked");
    done.store(true, std::memory_order_release);
    reader.join();
    for (std::size_t page = 0; page < pages; ++page) Require(GuestArena::GuestArenaDonated_nid_no_patch(start + page * Page), "a donated page is missing after the donation returned");
    GuestArena::GuestArenaWithdraw_nid_no_patch(start, pages * Page);
    Require(!GuestArena::GuestArenaDonated_nid_no_patch(start), "the concurrent range was not withdrawn");
}

}

int main() {
    try {
        SinglePages();
        ConcurrentReader();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "guest donation: %s\n", error.what());
        return 1;
    }
    std::puts("guest donation: ok");
    return 0;
}
