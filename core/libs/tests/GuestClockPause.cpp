#include "prx/libkernel/include/Api.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <chrono>
#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <thread>

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    const auto before = sceKernelGetProcessTime();
    KernelGuestClockPause_nid_no_patch(true);
    const auto pausedTime = sceKernelGetProcessTime();
    const auto pausedTsc = sceKernelReadTsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Require(sceKernelGetProcessTime() == pausedTime);
    Require(sceKernelReadTsc() == pausedTsc);
    KernelGuestClockPause_nid_no_patch(true);
    KernelGuestClockPause_nid_no_patch(false);
    const auto resumed = sceKernelGetProcessTime();
    Require(resumed >= pausedTime && resumed - pausedTime < 100000);
    Require(sceKernelReadTsc() >= pausedTsc);
    KernelGuestClockPause_nid_no_patch(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto later = sceKernelGetProcessTime();
    Require(later - before >= 40000 && later - before < 200000);

    std::atomic<bool> done{false};
    std::thread reader([&] {
        auto last = sceKernelGetProcessTimeCounter();
        auto lastTsc = sceKernelReadTsc();
        while (!done.load(std::memory_order_acquire)) {
            const auto now = sceKernelGetProcessTimeCounter();
            const auto tsc = sceKernelReadTsc();
            Require(now >= last && tsc >= lastTsc);
            last = now;
            lastTsc = tsc;
        }
    });
    for (int cycle = 0; cycle < 200; ++cycle) {
        KernelGuestClockPause_nid_no_patch(true);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        KernelGuestClockPause_nid_no_patch(false);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    return 0;
}
