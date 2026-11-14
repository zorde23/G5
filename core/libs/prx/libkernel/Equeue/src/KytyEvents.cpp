#include "prx/libkernel/Equeue/Equeue.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

std::atomic<std::uint64_t> g_graphics{0}, g_videoOut{0}, g_other{0}, g_refused{0};
std::atomic<std::int64_t> g_periodStart{0};

void NoteTrigger(std::int16_t filter, int result) {
    if (result != 0) g_refused.fetch_add(1, std::memory_order_relaxed);
    else if (filter == -14) g_graphics.fetch_add(1, std::memory_order_relaxed);
    else if (filter == -13) g_videoOut.fetch_add(1, std::memory_order_relaxed);
    else g_other.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto start = g_periodStart.load(std::memory_order_relaxed);
    if (start == 0) g_periodStart.compare_exchange_strong(start, now, std::memory_order_relaxed);
    else if (now - start >= 10 && g_periodStart.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        const auto take = [](std::atomic<std::uint64_t>& counter) { return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed)); };
        const auto graphics = take(g_graphics);
        const auto videoOut = take(g_videoOut);
        const auto other = take(g_other);
        std::fprintf(stderr, "[hib-eq] %lld s: %llu graphics, %llu video out, %llu other triggers; %llu refused\n", static_cast<long long>(now - start), graphics, videoOut, other, take(g_refused));
    }
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
    const int result = EqueueTriggerEvent_nid_postfix(eq, static_cast<uintptr_t>(ident), filter, triggerData);
    NoteTrigger(filter, result);
    return result;
}

}
