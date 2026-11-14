#include "prx/libkernel/include/Api.hpp"
#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>

extern "C" {
int KytyGpuAddEvent_nid_no_patch(KernelEqueue eq, std::uint64_t ident, std::int16_t filter, void* udata, int kind, int triggered, std::int64_t data);
int KytyGpuDeleteEvent_nid_no_patch(KernelEqueue eq, std::uint64_t ident, std::int16_t filter);
int KytyGpuTriggerEvent_nid_no_patch(KernelEqueue eq, std::uint64_t ident, std::int16_t filter, void* triggerData);
int KytyGpuMapDriverMemory_nid_no_patch(std::uint64_t address, std::uint64_t bytes, std::int64_t* physical);
int KytyGpuReleaseDriverMemory_nid_no_patch(std::int64_t physical, std::uint64_t bytes);
}

namespace {

constexpr int SceOk = 0;
constexpr std::int16_t EvfiltGraphics = -14;
constexpr std::int16_t EvfiltVideoOut = -13;

void Require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

int Wait(KernelEqueue eq, KernelEvent* events, int capacity) {
    int count = 0;
    const KernelUseconds timeout = 100000;
    const int result = sceKernelWaitEqueue(eq, events, capacity, &count, &timeout);
    return result == SceOk ? count : 0;
}

}

int main() {
    try {
        KernelEqueue eq = 0;
        int marker = 0;
        Require(sceKernelCreateEqueue(&eq, "kyty") == SceOk, "cannot create an event queue");
        Require(KytyGpuAddEvent_nid_no_patch(eq, 3, EvfiltGraphics, &marker, 0, 0, 0) == SceOk, "the graphics event was refused");
        Require(KytyGpuTriggerEvent_nid_no_patch(eq, 3, EvfiltGraphics, reinterpret_cast<void*>(std::intptr_t{5})) == SceOk, "the 1st interrupt was refused");
        Require(KytyGpuTriggerEvent_nid_no_patch(eq, 3, EvfiltGraphics, reinterpret_cast<void*>(std::intptr_t{5})) == SceOk, "the 2nd interrupt was refused");
        Require(KytyGpuTriggerEvent_nid_no_patch(eq, 3, EvfiltGraphics, reinterpret_cast<void*>(std::intptr_t{7})) == SceOk, "the 3rd interrupt was refused");
        KernelEvent events[4]{};
        Require(Wait(eq, events, 4) >= 1 && events[0].data == 5 && events[0].fflags == 2 && events[0].udata == &marker, "interrupts of one context did not add up in fflags with the context in data");
        Require(KytyGpuDeleteEvent_nid_no_patch(eq, 3, EvfiltGraphics) == SceOk, "the graphics event could not be deleted");
        Require(KytyGpuTriggerEvent_nid_no_patch(eq, 3, EvfiltGraphics, nullptr) != SceOk, "a deleted event still triggers");
        Require(KytyGpuAddEvent_nid_no_patch(eq, 0, EvfiltVideoOut, nullptr, 1, 0, 0) == SceOk, "the video out event was refused");
        Require(KytyGpuTriggerEvent_nid_no_patch(eq, 0, EvfiltVideoOut, reinterpret_cast<void*>(std::intptr_t{9})) == SceOk, "the flip was refused");
        Require(Wait(eq, events, 4) == 1 && events[0].fflags == 1 && ((static_cast<std::uint64_t>(events[0].data) >> 16u) == 9u) && ((static_cast<std::uint64_t>(events[0].data) >> 12u) & 0xfu) == 1u, "the flip event does not carry its payload and count");
        std::int64_t physical = -1;
        constexpr std::uint64_t driver = 0xfe0000000ull;
        Require(KytyGpuMapDriverMemory_nid_no_patch(driver, 0x200000, &physical) == 0 && physical >= 0, "the Kyty GPU driver memory was not mapped at 0xfe0000000");
        *reinterpret_cast<volatile std::uint32_t*>(driver) = 0x12345678;
        Require(*reinterpret_cast<volatile std::uint32_t*>(driver) == 0x12345678, "the driver memory does not hold a store");
        Require(KytyGpuReleaseDriverMemory_nid_no_patch(physical, 0x200000) == 0, "the driver memory was not released");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kyty events: %s\n", error.what());
        return 1;
    }
    std::puts("kyty events: ok");
    return 0;
}
