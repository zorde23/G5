#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::size_t BlockBytes = 0x10000;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

MEMORY_BASIC_INFORMATION Query(const void* pointer) {
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(pointer, &memory, sizeof(memory)) == sizeof(memory), "VirtualQuery failed");
    return memory;
}

struct Observation {
    std::uintptr_t address = 0;
    std::size_t bytes = 0;
    std::function<bool()> expected;
    bool covered = false;
    bool stateSeen = false;
    bool epochUnchanged = true;
    std::uint64_t epochBefore = 0;
};

Observation observation;

void Observe(std::uintptr_t address, std::size_t bytes) {
    if (address > observation.address || address + bytes < observation.address + observation.bytes) return;
    observation.covered = true;
    observation.stateSeen = observation.expected();
    observation.epochUnchanged = observation.epochUnchanged && GuestArena::GuestArenaCommitEpoch_nid_no_patch() == observation.epochBefore;
}

void CheckSite(const char* site, void* pointer, const std::function<void()>& operation, const std::function<bool()>& expected) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    std::uint64_t before[static_cast<std::size_t>(GuestArena::CommitEpochOrigin::Count)]{};
    GuestArena::GuestArenaCommitEpochCounts_nid_no_patch(before);
    observation = Observation{address, BlockBytes, expected};
    observation.epochBefore = GuestArena::GuestArenaCommitEpoch_nid_no_patch();
    const auto bucketBefore = GuestArena::GuestArenaBucketEpochs_nid_no_patch(address, BlockBytes);
    operation();
    const std::string name(site);
    Require(observation.covered, name + ": the forget callback did not cover the range");
    Require(observation.stateSeen, name + ": the forget callback ran before the new state was visible");
    Require(observation.epochUnchanged, name + ": the epoch rose before the forget callback");
    Require(GuestArena::GuestArenaCommitEpoch_nid_no_patch() > observation.epochBefore, name + ": the global epoch did not rise");
    Require(GuestArena::GuestArenaBucketEpochs_nid_no_patch(address, BlockBytes) > bucketBefore, name + ": the bucket epoch did not rise");
    GuestArena::CommitEpochEvent events[1]{};
    Require(GuestArena::GuestArenaCommitEpochEvents_nid_no_patch(events, 1) == 1 && events[0].epoch == GuestArena::GuestArenaCommitEpoch_nid_no_patch(), name + ": the event ring does not hold the last note");
    std::uint64_t after[static_cast<std::size_t>(GuestArena::CommitEpochOrigin::Count)]{};
    GuestArena::GuestArenaCommitEpochCounts_nid_no_patch(after);
    std::uint64_t total = 0;
    for (std::size_t origin = 0; origin < std::size(after); ++origin) total += after[origin] - before[origin];
    Require(total != 0, name + ": no origin counted the note");
}

HANDLE Section() {
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, BlockBytes, nullptr);
    Require(section != nullptr, "cannot create a section");
    return section;
}

bool Mapped(const void* pointer) {
    const auto memory = Query(pointer);
    return !(memory.State == MEM_RESERVE && memory.Type == MEM_PRIVATE);
}

bool Reserved(const void* pointer) {
    return Query(pointer).State == MEM_RESERVE;
}

void CheckSites() {
    auto* block = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    CheckSite("commit", block, [&] { GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes); }, [&] { return Query(block).State == MEM_COMMIT; });
    Require(GuestArena::GuestArenaPrivateCovers_nid_no_patch(address, BlockBytes), "a committed block is not private");
    Require(!GuestArena::GuestArenaPrivateCovers_nid_no_patch(address, BlockBytes * 2), "a range past the committed block is private");
    CheckSite("reset", block, [&] { GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes); }, [&] { return Reserved(block); });
    Require(!GuestArena::GuestArenaPrivateCovers_nid_no_patch(address, BlockBytes), "a reset block is still private");
    auto section = Section();
    CheckSite("map", block, [&] { GuestArena::GuestArenaMap_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE); }, [&] { return Mapped(block); });
    Require(!GuestArena::GuestArenaPrivateCovers_nid_no_patch(address, BlockBytes), "a shared view is private");
    CheckSite("set protection", block, [&] { GuestArena::GuestArenaSetProtection_nid_postfix(address, BlockBytes, PAGE_READONLY); }, [&] {
        std::uint32_t protection = 0;
        return GuestArena::GuestArenaProtection_nid_postfix(address, &protection) && protection == PAGE_READONLY;
    });
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    CheckSite("map direct", block, [&] { GuestArena::GuestArenaMapDirect_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE); }, [&] { return Mapped(block); });
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    CheckSite("map file", block, [&] { GuestArena::GuestArenaMapFile_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE, PAGE_READWRITE, BlockBytes); }, [&] { return Mapped(block); });
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    CloseHandle(section);
    GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    CheckSite("mutation", block, [&] { GuestAllocations::Mutation().Add(block, BlockBytes, true, true, false); }, [&] { return GuestAllocations::GuestAllocationsGeneration_nid_postfix() > generation; });
    GuestAllocations::Mutation().Remove(block);
    const auto invalidated = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    CheckSite("invalidate", block, [&] { GuestAllocations::GuestAllocationsInvalidate_nid_postfix(address, BlockBytes); }, [&] { return GuestAllocations::GuestAllocationsGeneration_nid_postfix() > invalidated; });
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
}

std::uint32_t State(std::uintptr_t address) {
    std::uint32_t protection = 0;
    Require(GuestArena::GuestArenaProtection_nid_postfix(address, &protection), "the stressed view has no protection");
    return (Query(reinterpret_cast<const void*>(address)).Protect << 8) | protection;
}

void CheckStress() {
    GuestArena::GuestArenaSetCommitEpochForget_nid_no_patch(nullptr);
    auto* block = GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes);
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    const auto section = Section();
    GuestArena::GuestArenaMap_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
    CloseHandle(section);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ended{0};
    std::thread mutator([&] {
        for (bool readOnly = true; !stop.load(std::memory_order_relaxed); readOnly = !readOnly) {
            GuestArena::GuestArenaSetProtection_nid_postfix(address, BlockBytes, readOnly ? PAGE_READONLY : PAGE_READWRITE);
            ended.fetch_add(1, std::memory_order_seq_cst);
        }
    });
    std::uint64_t observations = 0;
    std::uint64_t inFlight = 0;
    std::uint64_t lastEpoch = UINT64_MAX;
    std::uint32_t lastState = 0;
    std::uint64_t lastEnded = 0;
    std::string failure;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (failure.empty() && std::chrono::steady_clock::now() < deadline) {
        const auto endedBefore = ended.load(std::memory_order_seq_cst);
        const auto first = GuestArena::GuestArenaCommitEpoch_nid_no_patch();
        const auto state = State(address);
        const auto second = GuestArena::GuestArenaCommitEpoch_nid_no_patch();
        const auto endedAfter = ended.load(std::memory_order_seq_cst);
        if (first != second) continue;
        ++observations;
        if (first == lastEpoch && state != lastState) {
            if (endedBefore >= lastEnded + 2) failure = "the state changed at epoch " + std::to_string(first) + " across " + std::to_string(endedBefore - lastEnded) + " finished protection changes";
            else ++inFlight;
        }
        lastEpoch = first;
        lastState = state;
        lastEnded = endedAfter;
    }
    stop.store(true);
    mutator.join();
    GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
    Require(failure.empty(), failure);
    Require(observations != 0 && ended.load() != 0, "the stress made no observations");
    std::printf("commit epoch stress: %llu observations, %llu protection changes, %llu changes in flight\n", static_cast<unsigned long long>(observations), static_cast<unsigned long long>(ended.load()), static_cast<unsigned long long>(inFlight));
}

}

int main() {
    try {
        Require(GuestArena::GuestArenaAvailable_nid_postfix(), "the guest arena is unavailable");
        GuestArena::GuestArenaSetCommitEpochForget_nid_no_patch(&Observe);
        CheckSites();
        CheckStress();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "commit epoch: %s\n", error.what());
        return 1;
    }
    std::printf("commit epoch passed\n");
    return 0;
}
