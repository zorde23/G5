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
    return address >= base && address - base < size && bytes <= size - (address - base);
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

std::uint64_t CallGuest(std::uint64_t function, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t Resolve(const char* name) {
    static const std::map<std::string, void*> exports = {
        {"KytyGpuCallGuest_nid_no_patch", reinterpret_cast<void*>(&CallGuest)},
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

bool (*g_invalidate)(std::uint64_t, std::uint64_t) = nullptr;

void ApplyDeltas(const FrameCaptureReader& reader, std::uint64_t group) {
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Delta || chunk.group != group) continue;
        reader.Read(chunk, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes)));
        if (g_invalidate != nullptr) g_invalidate(chunk.address, chunk.bytes);
    }
}

struct Replayer {
    const FrameCaptureReader& reader;
    std::uint64_t (*resolve)(const char*);
    void (*waitIdle)();
    std::uint64_t frame = 0, calls = 0, callbacks = 0;
    std::uint64_t current = 0;
    std::set<const FrameCall*> used;
    std::set<const FrameCall*> done;

    std::map<std::uint64_t, const FrameCall*> objects;
    std::uint64_t differing = 0;

    std::uint64_t compared = 0, uncovered = 0;

    void Totals() {
        std::fprintf(stderr, "[kyty-replay] call objects so far: %llu compared, %llu differed (set to the game's words), %llu calls not covered\n", static_cast<unsigned long long>(compared), static_cast<unsigned long long>(differing), static_cast<unsigned long long>(uncovered));
        std::fflush(stderr);
    }

    void CheckObject(std::uint64_t ordinal, const char* nid, std::uint64_t rdi) {
        const auto found = objects.find(ordinal);
        if (found == objects.end() || found->second->values.size() < 3) {
            ++uncovered;
            std::fprintf(stderr, "[kyty-replay] call %llu %s: rdi %llx not covered\n", static_cast<unsigned long long>(ordinal), nid, static_cast<unsigned long long>(rdi));
            return;
        }
        ++compared;
        const auto& values = found->second->values;
        auto* object = reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(values[1]));
        const auto words = values.size() - 2;
        const bool equal = std::equal(values.begin() + 2, values.end(), object);
        if (equal) std::fprintf(stderr, "[kyty-replay] call %llu %s: object %llx equal\n", static_cast<unsigned long long>(ordinal), nid, static_cast<unsigned long long>(values[1]));
        if (!equal) {
            if (++differing <= 32) {
                std::fprintf(stderr, "[kyty-replay] call %llu %s: object %llu differs from the game:", static_cast<unsigned long long>(ordinal), nid, static_cast<unsigned long long>(values[1]));
                for (std::size_t i = 0; i < words; ++i) std::fprintf(stderr, " [%zu] %llx/%llx", i, static_cast<unsigned long long>(object[i]), static_cast<unsigned long long>(values[2 + i]));
                std::fputs(" (replay/game)\n", stderr);
            }
        }
        std::copy(values.begin() + 2, values.end(), object);
    }

    void Run() {
        for (const auto& call : reader.Calls()) {
            if (call.kind == FrameCallKind::GuestCallObject && !call.values.empty()) objects[call.values[0]] = &call;
        }
        for (const auto& call : reader.Calls()) {
            if (call.kind != FrameCallKind::GuestCallback) continue;
            std::fprintf(stderr, "[kyty-replay] recorded guest callback %llu: function %llu, object %llu, during call %llu on thread %llu, result %llu\n", static_cast<unsigned long long>(call.ordinal), static_cast<unsigned long long>(call.values[0]), static_cast<unsigned long long>(call.values[1]), static_cast<unsigned long long>(call.values.size() > 6 ? call.values[6] : 0), static_cast<unsigned long long>(call.thread), static_cast<unsigned long long>(call.values[4]));
        }
        for (const auto& call : reader.Calls()) {
            if (!done.contains(&call)) Step(call);
        }
        Totals();
        for (const auto& call : reader.Calls()) {
            if (call.kind == FrameCallKind::GuestCallback && !used.contains(&call)) std::fprintf(stderr, "[kyty-replay] recorded guest callback %llu was not asked for\n", static_cast<unsigned long long>(call.ordinal));
        }
    }

    std::uint64_t Callback(std::uint64_t function, std::uint64_t object) {
        for (const auto& call : reader.Calls()) {
            if (call.kind != FrameCallKind::GuestCallback || used.contains(&call) || call.values.size() < 6 || call.values[0] != function) continue;
            const bool keyed = call.values.size() > 7;
            if (keyed ? call.values[6] != current : call.values[1] != object) continue;
            used.insert(&call);
            std::fprintf(stderr, "[kyty-replay] guest callback %llu asked during call %llu: recorded %llu\n", static_cast<unsigned long long>(function), static_cast<unsigned long long>(current), static_cast<unsigned long long>(call.ordinal));
            if (keyed) {
                const auto trigger = current;
                bool inside = false;
                for (const auto& nested : reader.Calls()) {
                    if (&nested == &call) break;
                    if (nested.kind == FrameCallKind::GuestCall && nested.ordinal == trigger) inside = true;
                    if (!inside || nested.kind != FrameCallKind::GuestCall || nested.ordinal == trigger || nested.thread != call.thread || done.contains(&nested)) continue;
                    std::fprintf(stderr, "[kyty-replay] call %llu runs inside the callback of call %llu\n", static_cast<unsigned long long>(nested.ordinal), static_cast<unsigned long long>(trigger));
                    done.insert(&nested);
                    Step(nested);
                }
                current = trigger;
                std::copy(call.values.begin() + 8, call.values.end(), reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(call.values[1])));
            } else {
                ApplyDeltas(reader, call.values[5]);
            }
            ++callbacks;
            return call.values[4];
        }
        const auto* words = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(object));
        const auto found = objects.find(current);
        std::fprintf(stderr, "[kyty-replay] unmatched callback %llx during call %llu: object %llx (%s); replay words bottom %llx top %llx cursor_up %llx cursor_down %llx callback %llx user_data %llx reserved_dw %u\n", static_cast<unsigned long long>(function), static_cast<unsigned long long>(current), static_cast<unsigned long long>(object), found == objects.end() ? "no object recorded for this call" : (found->second->values[1] == object ? "the object compared before this call" : "not the object compared before this call"), static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]), static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]), static_cast<unsigned long long>(words[4]), static_cast<unsigned long long>(words[5]), static_cast<unsigned>(words[6] & 0xffffffffu));
        Totals();
        throw std::runtime_error("kyty frame replay: Kyty asked for the guest callback " + std::to_string(function) + " during call " + std::to_string(current) + ", which the capture does not have");
    }

    void Step(const FrameCall& call) {
        if (call.kind == FrameCallKind::FrameStart) {
            waitIdle();
            if (call.values.size() > 1) {
                if (call.values[1] != 0) ApplyDeltas(reader, call.values[1]);
            } else if (frame != 0) {
                ApplyDeltas(reader, frame);
            }
            ++frame;
            return;
        }
        if (call.kind == FrameCallKind::Memory) {
            waitIdle();
            if (!call.values.empty()) ApplyDeltas(reader, call.values[0]);
            return;
        }
        if (call.kind != FrameCallKind::GuestCall) return;
        current = call.ordinal;
        if (call.values.size() < StackAt) throw std::runtime_error("kyty frame replay: a guest call record is too short");
        char nid[NameWords * 8 + 1]{};
        std::memcpy(nid, call.values.data(), NameWords * 8);
        const auto target = resolve(nid);
        if (target == 0) throw std::runtime_error(std::string("kyty frame replay: the DLL does not export ") + nid);
        CheckObject(call.ordinal, nid, call.values[IntegerAt]);
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
};

Replayer* g_replayer = nullptr;

std::uint64_t CallGuest(std::uint64_t function, std::uint64_t object, std::uint64_t, std::uint64_t) {
    return g_replayer->Callback(function, object);
}

int List(const std::filesystem::path& directory) {
    const FrameCaptureReader reader(directory);
    for (const auto& call : reader.Calls()) {
        const auto name = AgcDriver::FrameCallName(call.kind);
        std::printf("%llu %.*s thread %llu", static_cast<unsigned long long>(call.ordinal), static_cast<int>(name.size()), name.data(), static_cast<unsigned long long>(call.thread));
        if (call.kind == FrameCallKind::GuestCall && call.values.size() >= StackAt) {
            char nid[NameWords * 8 + 1]{};
            std::memcpy(nid, call.values.data(), NameWords * 8);
            std::printf(" %s rdi %llx", nid, static_cast<unsigned long long>(call.values[IntegerAt]));
        } else {
            for (const auto value : call.values) std::printf(" %llx", static_cast<unsigned long long>(value));
        }
        std::printf("\n");
    }
    return 0;
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
            if (chunk.address >= arenaBase && chunk.address - arenaBase < arenaSize && chunk.bytes <= arenaSize - (chunk.address - arenaBase)) mutation.Add(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes), true, true, true);
        }
    }
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind == FrameMemoryKind::Snapshot) reader.Read(chunk, std::span(reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(chunk.address)), static_cast<std::size_t>(chunk.bytes)));
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
    Replayer replayer{reader, resolve, waitIdle};
    g_replayer = &replayer;
    static const Aps5Host host{2, &ProtectHost, &Resolve};
    init(&host);
    g_invalidate = reinterpret_cast<bool (*)(std::uint64_t, std::uint64_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_invalidate")));
    const auto mapInPlace = reinterpret_cast<void (*)(std::uint64_t, std::uint64_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_map_in_place")));
    const auto restoreState = reinterpret_cast<bool (*)(const std::uint64_t*, std::uint64_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_restore_state")));
    if (g_invalidate == nullptr || mapInPlace == nullptr || restoreState == nullptr) throw std::runtime_error("kyty frame replay: the DLL lacks kyty_gpu_invalidate, kyty_gpu_map_in_place or kyty_gpu_restore_state");
    std::uint64_t mapped = 0;
    for (const auto& chunk : reader.Chunks()) {
        if (chunk.kind != FrameMemoryKind::Range) continue;
        mapInPlace(chunk.address, chunk.bytes);
        ++mapped;
    }
    const auto state = std::find_if(reader.Calls().begin(), reader.Calls().end(), [](const FrameCall& call) { return call.kind == FrameCallKind::KytyState; });
    if (state == reader.Calls().end()) throw std::runtime_error("kyty frame replay: the capture has no Kyty state (made before the state export)");
    if (!restoreState(state->values.data(), state->values.size())) throw std::runtime_error("kyty frame replay: the DLL could not restore the captured Kyty state");
    std::fprintf(stderr, "[kyty-replay] %llu captured ranges mapped for the GPU in place, Kyty state restored\n", static_cast<unsigned long long>(mapped));
    if (const auto request = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_request_target_dump")))) request();
    replayer.Run();
    waitIdle();
    std::fprintf(stderr, "[kyty-replay] %llu frames, %llu guest calls, %llu guest callbacks replayed from %s\n", static_cast<unsigned long long>(replayer.frame), static_cast<unsigned long long>(replayer.calls), static_cast<unsigned long long>(replayer.callbacks), directory.string().c_str());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--list") return List(argv[2]);
    if (argc < 3) {
        std::fprintf(stderr, "usage: kyty_frame_replay <capture directory> <kyty_gpu.dll> [output directory, default <capture>/replay] | --list <capture directory>\n");
        return 2;
    }
    SetEnvironmentVariableA("DISABLE_RTSS_LAYER", "1");
    SetEnvironmentVariableA("DISABLE_VULKAN_OBS_CAPTURE", "1");
    try {
        const auto capture = std::filesystem::absolute(argv[1]);
        const int result = Replay(capture, std::filesystem::absolute(argv[2]), argc > 3 ? std::filesystem::absolute(argv[3]) : capture / "replay");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

#else

int main() {
    std::fprintf(stderr, "kyty_frame_replay is Windows only\n");
    return 1;
}

#endif
