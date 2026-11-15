#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/KytyGpu.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uintptr_t Base = 0x0000004000000000ull + GuestArena::GuestAddressOffset;
constexpr std::size_t Page = GuestArena::HostPageBytes;
constexpr std::size_t Bytes = 0x10000;

DWORD Protection(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    Require(VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != 0, "VirtualQuery failed");
    return info.Protect;
}

void ProtectionComesBack() {
    Require(KytyGpuEnabled(), "APS5_KYTY_GPU=1 is not set");
    auto* const pointer = reinterpret_cast<void*>(Base);
    GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, Bytes);
    GuestArena::GuestArenaCommit_nid_postfix(pointer, Bytes, PAGE_READWRITE, Bytes);
    Require(GuestArena::GuestArenaDonate_nid_no_patch(Base, Bytes), "the donation was refused");
    KytyGpuNoteGameProtection(Base, 3 * Page, PAGE_READWRITE);
    DWORD previous = 0;
    Require(VirtualProtect(reinterpret_cast<void*>(Base + Page), Page, PAGE_READONLY, &previous) != 0, "the game protection could not be set");
    KytyGpuNoteGameProtection(Base + Page, Page, PAGE_READONLY);
    Require(KytyGpuProtectHost(Base + 2 * Page, Page, 2), "the Kyty tracker protection was refused");
    const DWORD before[3] = {Protection(Base), Protection(Base + Page), Protection(Base + 2 * Page)};
    Require(before[0] == PAGE_READWRITE && before[1] == PAGE_READONLY && before[2] == PAGE_NOACCESS, "the pages did not start as read-write, read-only and no-access: " + std::to_string(before[0]) + " " + std::to_string(before[1]) + " " + std::to_string(before[2]));

    KytyGpuCaptureTrack_nid_no_patch(Base, 3 * Page, true);
    Require(Protection(Base) == PAGE_READONLY, "an armed writable page stayed writable");
    Require(Protection(Base + Page) == PAGE_READONLY && Protection(Base + 2 * Page) == PAGE_NOACCESS, "arming changed a page that was not writable");
    std::vector<std::uintptr_t> written;
    KytyGpuCaptureTakeWritten_nid_no_patch(Base, 3 * Page, &written);
    Require(written.empty(), "a page counts as written before any write");

    Require(KytyGpuHandleFault(Base + 8, 1, 0, nullptr), "the capture did not take the write fault of an armed page");
    Require(Protection(Base) == PAGE_READWRITE, "the written page was not released");
    Require(KytyGpuCaptureFaults_nid_no_patch() == 1, "the write fault was not counted");
    KytyGpuCaptureTakeWritten_nid_no_patch(Base, 3 * Page, &written);
    Require(written.size() == 1 && written.front() == Base, "the written page was not reported once");
    Require(Protection(Base) == PAGE_READONLY, "a reported page was not armed again");

    KytyGpuCaptureTrack_nid_no_patch(Base, 3 * Page, false);
    for (std::size_t index = 0; index < 3; ++index) Require(Protection(Base + index * Page) == before[index], "page " + std::to_string(index) + " did not get its protection back");
    KytyGpuCaptureTakeWritten_nid_no_patch(Base, 3 * Page, &written);
    Require(written.empty(), "a page counts as written after the capture stopped");
}

}

int main() {
    try {
        ProtectionComesBack();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kyty capture track: %s\n", error.what());
        return 1;
    }
    return 0;
}
