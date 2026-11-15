#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/KytyCapture.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace {

using AgcDriver::FrameCall;
using AgcDriver::FrameCallKind;
using AgcDriver::FrameCaptureReader;
using AgcDriver::FrameMemoryChunk;
using AgcDriver::FrameMemoryKind;

constexpr std::size_t NameWords = 2;
constexpr std::size_t IntegerAt = NameWords;
constexpr std::size_t VectorCountAt = IntegerAt + 6;
constexpr std::size_t XmmAt = VectorCountAt + 1;
constexpr std::size_t StackAt = XmmAt + 16;
constexpr std::uint64_t Granularity = 64u * 1024u;

struct Aps5Host {
    std::uint32_t version;
    bool (*protectHost)(std::uint64_t address, std::uint64_t bytes, std::uint32_t tracker);
    std::uint64_t (*resolve)(const char* nid);
};

std::uint64_t HeaderNumber(const std::string& header, const char* key) {
    const auto quoted = "\"" + std::string(key) + "\":";
    const auto at = header.find(quoted);
    if (at == std::string::npos) throw std::runtime_error(std::string("kyty frame replay: the capture header has no ") + key);
    return std::strtoull(header.c_str() + at + quoted.size(), nullptr, 10);
}

bool ProtectHost(std::uint64_t, std::uint64_t, std::uint32_t) {
    return false;
}

bool CommittedWritable(std::uint64_t address, std::uint64_t bytes) {
    for (auto at = address; at < address + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) return false;
        const auto access = info.Protect & 0xffu;
        if ((info.Protect & PAGE_GUARD) != 0 || (access != PAGE_READWRITE && access != PAGE_EXECUTE_READWRITE)) return false;
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

bool InArena(std::uint64_t address, std::uint64_t bytes) {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    GuestArena::GuestArenaRange_nid_postfix(&base, &size);
    return address >= base && address + bytes <= base + size;
}

void Commit(std::uint64_t address, std::uint64_t bytes, bool range);

int MapDriverMemory(std::uint64_t address, std::uint64_t bytes, std::int64_t* physical) {
    try {
        Commit(address, bytes, false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[kyty-replay] driver memory at %llu: %s\n", static_cast<unsigned long long>(address), error.what());
        return -1;
    }
    if (physical != nullptr) *physical = static_cast<std::int64_t>(address);
    return 0;
}

int ReleaseDriverMemory(std::int64_t, std::uint64_t) {
    return 0;
}

int NoEvent() {
    return 0;
}

bool NoPad(int, void*, std::uint64_t) {
    return false;
}

void Exit(int code) {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
}

int APS5_VABI RefusedKernel() {
    return static_cast<int>(0x80020016u);
}

std::uint64_t Resolve(const char* name) {
    static const std::map<std::string, void*> exports = {
        {"KytyGpuMapDriverMemory_nid_no_patch", reinterpret_cast<void*>(&MapDriverMemory)},
        {"KytyGpuReleaseDriverMemory_nid_no_patch", reinterpret_cast<void*>(&ReleaseDriverMemory)},
        {"KytyGpuAddEvent_nid_no_patch", reinterpret_cast<void*>(&NoEvent)},
        {"KytyGpuTriggerEvent_nid_no_patch", reinterpret_cast<void*>(&NoEvent)},
        {"KytyGpuDeleteEvent_nid_no_patch", reinterpret_cast<void*>(&NoEvent)},
        {"KytyGpuPadInjection_nid_no_patch", reinterpret_cast<void*>(&NoPad)},
        {"LibcRequestExit_nid_postfix", reinterpret_cast<void*>(&Exit)},
        {"rTXw65xmLIA", reinterpret_cast<void*>(&RefusedKernel)},
        {"pO96TwzOm5E", reinterpret_cast<void*>(&RefusedKernel)},
        {"NcaWUxfMNIQ", reinterpret_cast<void*>(&RefusedKernel)},
        {"MBuItvba6z8", reinterpret_cast<void*>(&RefusedKernel)},
    };
    const auto found = exports.find(name != nullptr ? name : "");
    if (found == exports.end()) std::fprintf(stderr, "[kyty-replay] the DLL asked the executable for %s: not provided\n", name != nullptr ? name : "(null)");
    return found == exports.end() ? 0 : reinterpret_cast<std::uint64_t>(found->second);
}

std::set<std::uint64_t> g_ownBlocks;

void Commit(std::uint64_t address, std::uint64_t bytes, bool range) {
    auto* const pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    if (InArena(address, bytes)) {
        if (CommittedWritable(address, bytes)) return;
        std::uintptr_t start = 0, end = 0;
        const bool overlaps = GuestArena::GuestArenaNextUsed_nid_no_patch(static_cast<std::uintptr_t>(address), &start, &end) && start < address + bytes;
        if (range && !overlaps) GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, static_cast<std::size_t>(bytes));
        GuestArena::GuestArenaCommit_nid_postfix(pointer, static_cast<std::size_t>(bytes), PAGE_READWRITE, static_cast<std::size_t>(bytes));
        return;
    }
    const auto begin = address & ~(Granularity - 1);
    const auto end = (address + bytes + Granularity - 1) & ~(Granularity - 1);
    for (auto block = begin; block < end; block += Granularity) {
        if (g_ownBlocks.contains(block)) continue;
        if (VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(block)), Granularity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == nullptr) {
            throw std::runtime_error("kyty frame replay: cannot map the captured memory at " + std::to_string(block) + " (the address is taken in this process)");
        }
        g_ownBlocks.insert(block);
    }
}

void ApplyDeltas(const FrameCaptureReader& reader, std::uint64_t group) {
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Delta || chunk.group != group) continue;
        reader.Read(chunk, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes)));
    }
}

int Replay(const std::filesystem::path& directory, const std::filesystem::path& dllPath, const std::filesystem::path& output) {
    const FrameCaptureReader reader(directory);
    if (reader.Truncated()) throw std::runtime_error("kyty frame replay: the capture is truncated");
    std::uintptr_t arenaBase = 0;
    std::size_t arenaSize = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaSize);
    if (HeaderNumber(reader.Header(), "arena_base") != arenaBase) throw std::runtime_error("kyty frame replay: the guest arena is at a different address than in the capture");
    {
        GuestAllocations::Mutation mutation;
        for (const auto& chunk : reader.Chunks()) {
            if (chunk.kind != FrameMemoryKind::Range) continue;
            Commit(chunk.address, chunk.bytes, true);
            if (chunk.address >= arenaBase && chunk.address + chunk.bytes <= arenaBase + arenaSize) mutation.Add(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes), true, true, true);
        }
    }
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Snapshot) reader.Read(chunk, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes)));
    }
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Range || (chunk.protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) continue;
        DWORD previous = 0;
        if (VirtualProtect(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<SIZE_T>(chunk.bytes), PAGE_EXECUTE_READWRITE, &previous) == 0) {
            throw std::runtime_error("kyty frame replay: cannot make the captured guest code at " + std::to_string(chunk.address) + " executable (error " + std::to_string(GetLastError()) + ")");
        }
    }
    for (const auto& call : reader.Calls()) {
        if (call.kind == FrameCallKind::GuestCall && call.size != 0) Commit(call.address, call.size, false);
    }
    std::filesystem::create_directories(output);
    std::filesystem::current_path(output);
    const auto dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr) throw std::runtime_error("kyty frame replay: cannot load " + dllPath.string());
    const auto init = reinterpret_cast<void (*)(const Aps5Host*)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_init")));
    const auto resolve = reinterpret_cast<std::uint64_t (*)(const char*)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_resolve")));
    const auto waitIdle = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_wait_idle")));
    if (init == nullptr || resolve == nullptr || waitIdle == nullptr) throw std::runtime_error("kyty frame replay: the DLL lacks kyty_gpu_init, kyty_gpu_resolve or kyty_gpu_wait_idle");
    static const Aps5Host host{2, &ProtectHost, &Resolve};
    init(&host);
    std::uint64_t frame = 0, calls = 0;
    for (const auto& call : reader.Calls()) {
        if (call.kind == FrameCallKind::FrameStart) {
            waitIdle();
            if (frame != 0) ApplyDeltas(reader, frame);
            ++frame;
            continue;
        }
        if (call.kind != FrameCallKind::GuestCall) continue;
        if (call.values.size() < StackAt) throw std::runtime_error("kyty frame replay: a guest call record is too short");
        char nid[NameWords * 8 + 1]{};
        std::memcpy(nid, call.values.data(), NameWords * 8);
        const auto target = resolve(nid);
        if (target == 0) throw std::runtime_error(std::string("kyty frame replay: the DLL does not export ") + nid);
        KytyCaptureReplayCall replay{};
        std::copy_n(call.values.begin() + IntegerAt, 6, replay.registers.integers);
        replay.registers.vectorCount = call.values[VectorCountAt];
        for (std::size_t i = 0; i < 8; ++i) {
            replay.registers.xmm[i][0] = call.values[XmmAt + 2 * i];
            replay.registers.xmm[i][1] = call.values[XmmAt + 2 * i + 1];
        }
        const auto stackWords = call.values.size() - StackAt;
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(call.address)), call.values.data() + StackAt, stackWords * 8);
        for (std::size_t i = 0; i < 8 && 1 + i < stackWords; ++i) replay.stackArguments[i] = call.values[StackAt + 1 + i];
        KytyCaptureReplay(replay, reinterpret_cast<void*>(static_cast<std::uintptr_t>(target)));
        ++calls;
    }
    waitIdle();
    std::fprintf(stderr, "[kyty-replay] %llu frames, %llu guest calls replayed from %s\n", static_cast<unsigned long long>(frame), static_cast<unsigned long long>(calls), directory.string().c_str());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: kyty_frame_replay <capture directory> <kyty_gpu.dll> [output directory, default <capture>/replay]\n");
        return 2;
    }
    try {
        const auto capture = std::filesystem::absolute(argv[1]);
        const int result = Replay(capture, std::filesystem::absolute(argv[2]), argc > 3 ? std::filesystem::absolute(argv[3]) : capture / "replay");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}

#else

int main() {
    std::fprintf(stderr, "kyty_frame_replay is Windows only\n");
    return 1;
}

#endif
