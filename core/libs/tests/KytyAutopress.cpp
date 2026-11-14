#include "prx/libScePad/include/Pad.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" bool KytyGpuPadInjection_nid_no_patch(int port, PadData* out, std::size_t bytes);

int main() {
    PadData data{};
    if (!KytyGpuPadInjection_nid_no_patch(0, &data, sizeof(data)) || (data.buttons & 0x4000u) == 0) {
        std::fputs("kyty autopress: cross is not pressed at the start of the phase\n", stderr);
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (KytyGpuPadInjection_nid_no_patch(0, &data, sizeof(data))) {
        std::fputs("kyty autopress: the pad is still injected between presses\n", stderr);
        return 1;
    }
    std::puts("kyty autopress: ok");
    return 0;
}
