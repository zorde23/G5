#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

void PinWorkerThread(const char* role) {
    TracyZone::NameThread(role);
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(role);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", role, static_cast<unsigned long>(nameResult));
#endif
    auto mask = WorkerAffinityMask();
    if (mask == 0 && CpuTopology::ReservedCore() != 0) mask = CpuTopology::Get().process;
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] queue workers and presenter -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx)\n", static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

bool PinCommitterThread() {
    return CpuTopology::PinToReservedCore("draw committer") != 0;
}

void PinFinisherThread() {
    if (CpuTopology::ReservedCore() != 0) CpuTopology::PinTraced("draw finisher", nullptr, CpuTopology::Get().process);
}

void NoteCommitterCore(std::chrono::steady_clock::time_point now) {
#ifdef _WIN32
    static std::atomic<std::uint64_t> items{0}, offCore{0};
    const auto tenSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() / 10;
    static std::atomic<std::int64_t> reportedAt{tenSeconds};
    items.fetch_add(1, std::memory_order_relaxed);
    if (((CpuTopology::ReservedCore() >> GetCurrentProcessorNumber()) & 1u) == 0) offCore.fetch_add(1, std::memory_order_relaxed);
    if (reportedAt.exchange(tenSeconds, std::memory_order_relaxed) == tenSeconds) return;
    std::fprintf(stderr, "[committer-core] 10 s: %llu items, %llu of them ran off the reserved core 0x%llx\n", static_cast<unsigned long long>(items.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(offCore.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(CpuTopology::ReservedCore()));
#else
    (void)now;
#endif
}

}
