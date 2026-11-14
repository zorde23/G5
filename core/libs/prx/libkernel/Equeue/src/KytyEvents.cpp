#include "prx/libkernel/Equeue/Equeue.hpp"
#include <cstdint>
#include <x86intrin.h>

namespace {

void ResetCount(KernelEqueueEvent* event) {
    event->triggered = false;
    event->event.fflags = 0;
    event->event.data = 0;
}

void TriggerInterrupt(KernelEqueueEvent* event, void* triggerData) {
    const auto data = reinterpret_cast<intptr_t>(triggerData);
    if (event->triggered) {
        if (event->event.data == data) {
            event->event.fflags++;
            return;
        }
        for (auto& pending : event->pendingEvents) {
            if (pending.data == data) {
                pending.fflags++;
                return;
            }
        }
        auto triggered = event->event;
        triggered.fflags = 1;
        triggered.data = data;
        event->pendingEvents.push_back(triggered);
        return;
    }
    event->event.fflags++;
    event->event.data = data;
    event->triggered = true;
}

intptr_t VideoOutData(intptr_t current, void* triggerData) {
    std::uint64_t counter = (static_cast<std::uint64_t>(current) >> 12u) & 0xfu;
    if (counter != 0xfu) counter++;
    const std::uint64_t payload = static_cast<std::uint64_t>(reinterpret_cast<intptr_t>(triggerData));
    return static_cast<intptr_t>((__rdtsc() & 0xfffu) | (counter << 12u) | ((payload & 0x0000ffffffffffffull) << 16u));
}

void TriggerVideoOut(KernelEqueueEvent* event, void* triggerData) {
    auto triggered = event->event;
    triggered.fflags = triggered.fflags < 0xfu ? triggered.fflags + 1u : triggered.fflags;
    triggered.data = VideoOutData(triggered.data, triggerData);
    if (event->triggered) {
        event->pendingEvents.push_back(triggered);
        return;
    }
    event->event = triggered;
    event->triggered = true;
}

}

extern "C" {

int KytyGpuAddEvent_nid_no_patch(KernelEqueue eq, std::uint64_t ident, std::int16_t filter, void* udata, int kind, int triggered, std::int64_t data) {
    KernelEqueueEvent event{};
    event.triggered = triggered != 0;
    event.event.ident = static_cast<uintptr_t>(ident);
    event.event.filter = filter;
    event.event.udata = udata;
    event.event.fflags = triggered != 0 ? 1u : 0u;
    event.event.data = static_cast<intptr_t>(data);
    event.filter.resetFunc = ResetCount;
    event.filter.triggerFunc = kind == 1 ? TriggerVideoOut : TriggerInterrupt;
    return EqueueAddEvent_nid_postfix(eq, event);
}

int KytyGpuDeleteEvent_nid_no_patch(KernelEqueue eq, std::uint64_t ident, std::int16_t filter) {
    return EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(ident), filter);
}

int KytyGpuTriggerEvent_nid_no_patch(KernelEqueue eq, std::uint64_t ident, std::int16_t filter, void* triggerData) {
    return EqueueTriggerEvent_nid_postfix(eq, static_cast<uintptr_t>(ident), filter, triggerData);
}

}
