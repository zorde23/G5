#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/KytyCapture.hpp"
#include "prx/libc/include/KytyGpu.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace {

using AgcDriver::FrameCall;
using AgcDriver::FrameCallKind;
using AgcDriver::FrameCaptureWriter;
using AgcDriver::FrameRangeKind;

constexpr std::uint64_t PageBytes = 4096;
constexpr std::uint64_t StackBytes = 4096;
constexpr std::size_t CallbackObjectWords = 7;
constexpr std::uint32_t FullIdleTimeoutMs = 1000;
constexpr std::uint32_t GfxIdleTimeoutMs = 4000;

thread_local std::uint64_t t_callOrdinal = 0;
thread_local bool t_setFlip = false;
constexpr const char* SetFlipNid = "YUeqkyT7mEQ";
constexpr const char* SubmitFlipNid = "U46NwOiJpys";

struct Range {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::uint32_t protection = 0;
    bool writable = false;
    bool donated = false;
};

std::uint64_t GuestReadableBytes(std::uint64_t address, std::uint64_t limit) {
    if (address == 0) return 0;
    std::uint64_t end = 0;
    if (const auto allocation = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(address), false); allocation != nullptr) {
        if (!allocation->readable) return 0;
        end = allocation->address + allocation->bytes;
    } else {
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const auto low = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        const auto high = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        if (address < low || address >= high) return 0;
        end = high;
    }
    const auto bytes = std::min<std::uint64_t>(limit, end - address) & ~std::uint64_t{7};
    return bytes != 0 && KytyGpuReadableWithoutFault_nid_no_patch(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(bytes)) ? bytes : 0;
}

bool IsSubmit(const char* nid) {
    static constexpr const char* submits[] = {"UglJIZjGssM", "AhGvpITrf4M", "6UzEidRZwkg", "+T8Xo6LtFJI", "b4fpgH5ZXxQ", "Fj7r9EHzF38", "gSRnr79F8tQ", "HF3YllT3mXU", "U46NwOiJpys"};
    return std::any_of(std::begin(submits), std::end(submits), [nid](const char* submit) { return std::strcmp(nid, submit) == 0; });
}

class Recorder {
public:
    static Recorder* Active() {
        static Recorder* const recorder = []() -> Recorder* {
            const char* directory = std::getenv("APS5_KYTY_CAPTURE");
            if (directory == nullptr || *directory == '\0') return nullptr;
            const char* trigger = std::getenv("APS5_KYTY_CAPTURE_TRIGGER");
            const char* frames = std::getenv("APS5_KYTY_CAPTURE_FRAMES");
            return new Recorder(directory, trigger != nullptr ? trigger : "", frames != nullptr && std::atoi(frames) > 0 ? static_cast<std::uint64_t>(std::atoi(frames)) : 3);
        }();
        return recorder;
    }

    static bool Abandoned() { return abandoned.load(); }

    static void Abandon(const char* reason) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) std::fprintf(stderr, "[kyty-capture] stopped: %s\n", reason);
        abandoned.store(true);
        try {
            if (auto* recorder = Active()) {
                std::lock_guard lock(recorder->mutex);
                recorder->disarm();
            }
        } catch (...) {
            std::fputs("[kyty-capture] the donated pages could not be released from the capture write protection\n", stderr);
        }
    }

    void Call(const char* nid, const KytyCaptureRegisters& registers) {
        if (abandoned.load()) return;
        std::unique_lock lock(mutex);
        if (state == State::Done) return;
        if (starting) {
            ++startingSeen;
            if (!started.wait_for(lock, std::chrono::seconds(2), [this] { return !starting; })) {
                ++startingPassed;
                return;
            }
            if (state == State::Done) return;
        }
        const auto flips = flipCount != nullptr ? flipCount() : 0;
        if (flips != lastFlips) {
            lastFlips = flips;
            if (boundary() && !pending) {
                pending = true;
                step("armed: starting after the next frame's flip submit on the presenting thread");
            }
        }
        const auto thread = GetCurrentThreadId();
        if (state == State::Waiting && startThread == 0) {
            if (std::strcmp(nid, SetFlipNid) == 0) t_setFlip = true;
            const bool flipSubmit = std::strcmp(nid, SubmitFlipNid) == 0 || (t_setFlip && IsSubmit(nid));
            if (flipSubmit) t_setFlip = false;
            if (pending && flipSubmit) {
                startThread = thread;
                step("flip submitted: the capture starts at this thread's next call");
            }
            return;
        }
        if (state == State::Waiting && pending && startThread == thread) {
            {
                pending = false;
                startThread = 0;
                starting = true;
                startingSeen = startingPassed = 0;
                lock.unlock();
                step("waiting for the whole GPU to go idle");
                const bool full = waitIdleFor != nullptr ? waitIdleFor(FullIdleTimeoutMs) : (waitIdle(), true);
                bool idle = full;
                runningQueues.clear();
                if (!full && waitGfxIdleFor != nullptr) {
                    step("the whole GPU is still busy: waiting for the graphics queue only");
                    idle = waitGfxIdleFor(GfxIdleTimeoutMs);
                    if (idle && busyQueues != nullptr) {
                        std::uint32_t queues[64]{};
                        const auto count = std::min<std::uint32_t>(busyQueues(queues, 64), 64);
                        runningQueues.assign(queues, queues + count);
                    }
                }
                lock.lock();
                idleRule = full ? "full idle" : "gfx0 idle, compute running";
                if (!idle) {
                    starting = false;
                    started.notify_all();
                    lastFlips = flipCount != nullptr ? flipCount() : 0;
                    step(trigger == "F9" ? "neither the whole GPU (1 s) nor the graphics queue (4 s) went idle: not started, press F9 again" : "neither the whole GPU (1 s) nor the graphics queue (4 s) went idle: not started, trying at the next flip");
                    return;
                }
                step(full ? "GPU idle: full idle" : "GPU idle: gfx0 idle, compute running");
                try {
                    start();
                } catch (...) {
                    starting = false;
                    started.notify_all();
                    throw;
                }
                starting = false;
                started.notify_all();
                if (startingPassed != 0) throw std::runtime_error("APS5_KYTY_CAPTURE: " + std::to_string(startingPassed) + " calls ran unrecorded while the GPU went idle for the snapshot");
                std::fprintf(stderr, "[kyty-capture] %llu calls waited for the snapshot\n", static_cast<unsigned long long>(startingSeen));
            }
        }
        if (state != State::Recording) return;
        if (IsSubmit(nid)) submitDelta();
        record(nid, registers);
    }

    void Callback(std::uint64_t function, std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t result, std::uint64_t trigger) {
        if (abandoned.load()) return;
        std::lock_guard lock(mutex);
        if (state != State::Recording) return;
        FrameCall call;
        call.kind = FrameCallKind::GuestCallback;
        call.ordinal = ++ordinal;
        call.timeNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - origin).count());
        call.thread = GetCurrentThreadId();
        call.values = {function, first, second, third, result, 0, trigger, call.thread};
        if (inWritableRange(first, CallbackObjectWords * 8)) {
            const auto* object = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(first));
            call.values.insert(call.values.end(), object, object + CallbackObjectWords);
        }
        writer->WriteCall(call);
        ++recordedCallbacks;
    }

private:
    enum class State { Waiting, Recording, Done };

    Recorder(std::filesystem::path directory, std::filesystem::path trigger, std::uint64_t frames) : directory(std::move(directory)), trigger(std::move(trigger)), frames(frames), origin(std::chrono::steady_clock::now()) {
        const char* path = std::getenv("APS5_KYTY_GPU_DLL");
        const auto dll = GetModuleHandleA(std::filesystem::path(path != nullptr ? path : "kyty_gpu.dll").filename().string().c_str());
        flipCount = reinterpret_cast<std::uint64_t (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_flip_count")));
        waitIdle = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_wait_idle")));
        requestTargetDump = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_request_target_dump")));
        captureState = reinterpret_cast<std::uint64_t (*)(std::uint64_t*, std::uint64_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_capture_state")));
        takeHotkey = reinterpret_cast<bool (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_take_capture_hotkey")));
        waitIdleFor = reinterpret_cast<bool (*)(std::uint32_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_wait_idle_for")));
        waitGfxIdleFor = reinterpret_cast<bool (*)(std::uint32_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_wait_gfx_idle_for")));
        busyQueues = reinterpret_cast<std::uint32_t (*)(std::uint32_t*, std::uint32_t)>(reinterpret_cast<void*>(GetProcAddress(dll, "kyty_gpu_busy_queues")));
        if (this->trigger == "F9" && takeHotkey == nullptr) throw std::runtime_error("APS5_KYTY_CAPTURE_TRIGGER=F9: the Kyty GPU DLL has no kyty_gpu_take_capture_hotkey");
        const char* at = std::getenv("APS5_KYTY_CAPTURE_AT_FLIP");
        atFlip = at != nullptr ? std::strtoull(at, nullptr, 10) : 0;
        if (flipCount == nullptr || waitIdle == nullptr) throw std::runtime_error("APS5_KYTY_CAPTURE: the Kyty GPU DLL has no kyty_gpu_flip_count or kyty_gpu_wait_idle");
        std::fprintf(stderr, "[kyty-capture] armed: %s, %llu frames from flip %llu, trigger %s\n", this->directory.string().c_str(), static_cast<unsigned long long>(frames), static_cast<unsigned long long>(atFlip), this->trigger.empty() ? "(none)" : this->trigger.string().c_str());
    }

    bool boundary() {
        if (state == State::Recording) {
            emitDelta();
            if (++recordedFrames >= frames) finish();
            else writeMarker(FrameCallKind::FrameStart);
            return false;
        }
        if (lastFlips < atFlip) return false;
        if (trigger == "F9") return takeHotkey != nullptr && takeHotkey();
        std::error_code error;
        return trigger.empty() || std::filesystem::exists(trigger, error);
    }

    void start() {
        const auto began = std::chrono::steady_clock::now();
        writer = std::make_unique<FrameCaptureWriter>(directory);
        step("folder created");
        writeKytyState();
        ranges = committedRanges();
        for (auto& range : ranges) {
            if (!range.writable) continue;
            range.donated = GuestArena::GuestArenaDonatedAny_nid_no_patch(static_cast<std::uintptr_t>(range.address), static_cast<std::size_t>(range.bytes));
            if (!range.donated) {
                writableBytes += range.bytes;
                continue;
            }
            donatedBytes += range.bytes;
            KytyGpuCaptureTrack_nid_no_patch(static_cast<std::uintptr_t>(range.address), static_cast<std::size_t>(range.bytes), true);
        }
        armed = true;
        KytyGpuCaptureFaults_nid_no_patch();
        step("snapshot");
        std::vector<std::byte> bytes;
        for (const auto& range : ranges) {
            writer->WriteRange(range.address, range.bytes, FrameRangeKind::Section, range.protection, AgcDriver::FrameRangeAccess::Readable | (range.writable ? AgcDriver::FrameRangeAccess::Writable : 0u));
            for (std::uint64_t at = 0; at < range.bytes; at += FrameCaptureWriter::ChunkBytes) {
                bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(FrameCaptureWriter::ChunkBytes, range.bytes - at)));
                std::memcpy(bytes.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(range.address + at)), bytes.size());
                snapshotBytes += writer->WriteSnapshot(range.address + at, bytes);
                if (range.writable) hashPages(range.address + at, bytes, false);
            }
        }
        state = State::Recording;
        recordedFrames = 0;
        writeMarker(FrameCallKind::FrameStart);
        if (requestTargetDump != nullptr) requestTargetDump();
        lastFlips = flipCount();
        std::fprintf(stderr, "[kyty-capture] started: %zu ranges, %.1f MiB stored in %.1f s\n", ranges.size(), static_cast<double>(snapshotBytes) / (1024.0 * 1024.0), std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
    }

    void hashPages(std::uint64_t address, std::span<const std::byte> bytes, bool emit) {
        for (std::size_t at = 0; at + PageBytes <= bytes.size(); at += PageBytes) {
            const auto hash = AgcDriver::FrameHash(bytes.data() + at, PageBytes);
            auto& known = pageHashes[address + at];
            if (known == hash) continue;
            known = hash;
            if (emit) {
                writer->WriteDelta(deltaGroup, 0, address + at, bytes.subspan(at, PageBytes));
                deltaBytes += PageBytes;
            }
        }
    }

    void submitDelta() {
        const auto began = std::chrono::steady_clock::now();
        const auto before = deltaBytes;
        emitDelta();
        FrameCall call;
        call.kind = FrameCallKind::Memory;
        call.ordinal = ++ordinal;
        call.values = {deltaGroup};
        writer->WriteCall(call);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        ++submits;
        submitMs += ms;
        submitMaxMs = std::max(submitMaxMs, ms);
        if (submits <= 16) std::fprintf(stderr, "[kyty-capture] submit %llu: %.1f MiB not donated hashed, %zu of %.1f MiB donated pages written (%llu write faults), %.1f KiB changed, %.2f ms\n", static_cast<unsigned long long>(submits), static_cast<double>(writableBytes) / (1024.0 * 1024.0), writtenPages, static_cast<double>(donatedBytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(KytyGpuCaptureFaults_nid_no_patch()), static_cast<double>(deltaBytes - before) / 1024.0, ms);
    }

    void emitDelta() {
        ++deltaGroup;
        writtenPages = 0;
        std::vector<std::byte> bytes;
        for (const auto& range : ranges) {
            if (range.donated) {
                std::vector<std::uintptr_t> written;
                KytyGpuCaptureTakeWritten_nid_no_patch(static_cast<std::uintptr_t>(range.address), static_cast<std::size_t>(range.bytes), &written);
                writtenPages += written.size();
                bytes.resize(PageBytes);
                for (const auto page : written) {
                    std::memcpy(bytes.data(), reinterpret_cast<const void*>(page), PageBytes);
                    hashPages(page, bytes, true);
                }
                continue;
            }
            if (!range.writable || !stillReadable(range)) continue;
            for (std::uint64_t at = 0; at < range.bytes; at += FrameCaptureWriter::ChunkBytes) {
                bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(FrameCaptureWriter::ChunkBytes, range.bytes - at)));
                std::memcpy(bytes.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(range.address + at)), bytes.size());
                hashPages(range.address + at, bytes, true);
            }
        }
    }

    bool inWritableRange(std::uint64_t address, std::uint64_t bytes) const {
        return std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) { return range.writable && address >= range.address && address - range.address < range.bytes && bytes <= range.bytes - (address - range.address); });
    }

    void recordObject(std::uint64_t callOrdinal, std::uint64_t address) {
        if (!inWritableRange(address, CallbackObjectWords * 8)) return;
        FrameCall call;
        call.kind = FrameCallKind::GuestCallObject;
        call.ordinal = ++ordinal;
        call.values = {callOrdinal, address};
        const auto* object = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(address));
        call.values.insert(call.values.end(), object, object + CallbackObjectWords);
        writer->WriteCall(call);
    }

    void step(const char* what) const {
        std::fprintf(stderr, "[kyty-capture] %.3f s: %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(), what);
        std::fflush(stderr);
    }

    void writeKytyState() {
        if (captureState == nullptr) throw std::runtime_error("APS5_KYTY_CAPTURE: the Kyty GPU DLL has no kyty_gpu_capture_state, so a replay could not restore its state");
        FrameCall call;
        call.kind = FrameCallKind::KytyState;
        call.ordinal = ++ordinal;
        call.values.resize(static_cast<std::size_t>(captureState(nullptr, 0)));
        if (call.values.size() < 2 || captureState(call.values.data(), call.values.size()) != call.values.size()) throw std::runtime_error("APS5_KYTY_CAPTURE: the Kyty GPU state could not be read");
        writer->WriteCall(call);
        std::fprintf(stderr, "[kyty-capture] Kyty state: %llu shaders, %.1f KiB\n", static_cast<unsigned long long>(call.values[1]), static_cast<double>(call.values.size() * 8) / 1024.0);
    }

    FrameCall makeCall(const char* nid, const KytyCaptureRegisters& registers) {
        FrameCall call;
        call.kind = FrameCallKind::GuestCall;
        call.timeNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - origin).count());
        call.thread = GetCurrentThreadId();
        call.address = registers.stack - 8;
        call.size = StackBytes;
        std::uint64_t name[2]{};
        std::memcpy(name, nid, std::min<std::size_t>(std::strlen(nid), sizeof(name)));
        call.values.assign(std::begin(name), std::end(name));
        call.values.insert(call.values.end(), std::begin(registers.integers), std::end(registers.integers));
        call.values.push_back(registers.vectorCount);
        for (const auto& xmm : registers.xmm) call.values.insert(call.values.end(), std::begin(xmm), std::end(xmm));
        const auto stackBytes = GuestReadableBytes(call.address, StackBytes);
        call.size = stackBytes;
        const auto* stack = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(call.address));
        call.values.insert(call.values.end(), stack, stack + stackBytes / 8);
        return call;
    }

    void record(const char* nid, const KytyCaptureRegisters& registers) {
        auto call = makeCall(nid, registers);
        call.ordinal = ++ordinal;
        writer->WriteCall(call);
        t_callOrdinal = call.ordinal;
        recordObject(call.ordinal, registers.integers[0]);
        ++recordedCalls;
    }

    void writeMarker(FrameCallKind kind) {
        FrameCall call;
        call.kind = kind;
        call.ordinal = ++ordinal;
        call.values = {lastFlips, deltaGroup};
        writer->WriteCall(call);
    }

    void disarm() {
        if (!armed) return;
        armed = false;
        for (const auto& range : ranges) {
            if (range.donated) KytyGpuCaptureTrack_nid_no_patch(static_cast<std::uintptr_t>(range.address), static_cast<std::size_t>(range.bytes), false);
        }
    }

    void finish() {
        disarm();
        writeMarker(FrameCallKind::End);
        std::uintptr_t arenaBase = 0;
        std::size_t arenaSize = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaSize);
        char header[512];
        std::snprintf(header, sizeof(header), "{\"kind\":\"kyty-guest-calls\",\"arena_base\":%llu,\"frames\":%llu,\"calls\":%llu,\"callbacks\":%llu,\"ranges\":%zu,\"snapshot_bytes\":%llu,\"delta_bytes\":%llu,\"stack_bytes\":%llu,\"idle_rule\":\"%s\",\"running_queues\":[", static_cast<unsigned long long>(arenaBase), static_cast<unsigned long long>(recordedFrames), static_cast<unsigned long long>(recordedCalls), static_cast<unsigned long long>(recordedCallbacks), ranges.size(), static_cast<unsigned long long>(snapshotBytes), static_cast<unsigned long long>(deltaBytes), static_cast<unsigned long long>(StackBytes), idleRule.c_str());
        std::string json(header);
        for (std::size_t i = 0; i < runningQueues.size(); ++i) json += (i == 0 ? "" : ",") + std::to_string(runningQueues[i]);
        json += runningQueues.empty() ? "]}" : "],\"gap\":\"asynchronous compute was running when the snapshot was taken\"}";
        writer->Finish(json);
        std::fprintf(stderr, "[kyty-capture] %llu frames, %llu guest calls, %llu guest callbacks, %.1f MiB of deltas, stored in %s\n", static_cast<unsigned long long>(recordedFrames), static_cast<unsigned long long>(recordedCalls), static_cast<unsigned long long>(recordedCallbacks), static_cast<double>(deltaBytes) / (1024.0 * 1024.0), directory.string().c_str());
        if (submits != 0) std::fprintf(stderr, "[kyty-capture] %llu submit deltas: %.2f ms mean, %.2f ms max\n", static_cast<unsigned long long>(submits), submitMs / static_cast<double>(submits), submitMaxMs);
        writer.reset();
        state = State::Done;
    }

    static bool stillReadable(const Range& range) {
        for (auto address = range.address; address < range.address + range.bytes;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) return false;
            const auto page = info.Protect & 0xffu;
            const bool readable = (info.Protect & PAGE_GUARD) == 0 && page != PAGE_NOACCESS && page != 0;
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (!readable && !GuestArena::GuestArenaDonatedAny_nid_no_patch(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(std::min<std::uint64_t>(end, range.address + range.bytes) - address))) return false;
            address = end;
        }
        return true;
    }

    static std::vector<Range> committedRanges() {
        std::vector<Range> result;
        for (const auto& allocation : GuestAllocations::GuestAllocationsAcquireAll_nid_postfix()) {
            if (!allocation->readable) continue;
            const auto end = allocation->address + allocation->bytes;
            for (auto address = allocation->address; address < end;) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), &info, sizeof(info)) == 0) break;
                const auto regionEnd = std::min<std::uint64_t>(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
                const auto page = info.Protect & 0xffu;
                const bool readable = (info.Protect & PAGE_GUARD) == 0 && page != PAGE_NOACCESS && page != 0;
                const bool donated = GuestArena::GuestArenaDonatedAny_nid_no_patch(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(regionEnd - address));
                if (info.State == MEM_COMMIT && (readable || donated)) result.push_back({address, regionEnd - address, info.Protect, allocation->writable, false});
                address = regionEnd;
            }
        }
        return result;
    }

    static inline std::atomic<bool> abandoned{false};
    std::mutex mutex;
    std::filesystem::path directory;
    std::filesystem::path trigger;
    std::uint64_t frames = 3;
    std::uint64_t atFlip = 0;
    std::chrono::steady_clock::time_point origin;
    std::uint64_t (*flipCount)() = nullptr;
    void (*waitIdle)() = nullptr;
    void (*requestTargetDump)() = nullptr;
    std::uint64_t (*captureState)(std::uint64_t*, std::uint64_t) = nullptr;
    bool (*takeHotkey)() = nullptr;
    bool (*waitIdleFor)(std::uint32_t) = nullptr;
    bool (*waitGfxIdleFor)(std::uint32_t) = nullptr;
    std::uint32_t (*busyQueues)(std::uint32_t*, std::uint32_t) = nullptr;
    std::string idleRule = "full idle";
    std::vector<std::uint32_t> runningQueues;
    bool pending = false;
    DWORD startThread = 0;
    std::condition_variable started;
    std::uint64_t startingSeen = 0, startingPassed = 0;
    State state = State::Waiting;
    bool starting = false;
    std::uint64_t lastFlips = 0;
    std::uint64_t recordedFrames = 0;
    std::uint64_t recordedCalls = 0;
    std::uint64_t recordedCallbacks = 0;
    std::uint64_t submits = 0;
    std::uint64_t writableBytes = 0;
    std::uint64_t donatedBytes = 0;
    double submitMs = 0;
    double submitMaxMs = 0;
    std::size_t writtenPages = 0;
    bool armed = false;
    std::uint64_t ordinal = 0;
    std::uint64_t deltaGroup = 0;
    std::uint64_t snapshotBytes = 0;
    std::uint64_t deltaBytes = 0;
    std::unique_ptr<FrameCaptureWriter> writer;
    std::vector<Range> ranges;
    std::unordered_map<std::uint64_t, std::uint64_t> pageHashes;
};

}

extern "C" void AgcDriverKytyCaptureCall_nid_no_patch(const char* nid, const KytyCaptureRegisters* registers) {
    if (Recorder::Abandoned()) return;
    try {
        if (auto* recorder = Recorder::Active(); recorder != nullptr && nid != nullptr && registers != nullptr) recorder->Call(nid, *registers);
    } catch (const std::exception& error) {
        Recorder::Abandon(error.what());
    } catch (...) {
        Recorder::Abandon("unknown error");
    }
}

extern "C" std::uint64_t AgcDriverKytyCaptureCallbackTrigger_nid_no_patch() {
    return t_callOrdinal;
}

extern "C" void AgcDriverKytyCaptureCallback_nid_no_patch(std::uint64_t function, std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t result, std::uint64_t trigger) {
    if (Recorder::Abandoned()) return;
    try {
        if (auto* recorder = Recorder::Active()) recorder->Callback(function, first, second, third, result, trigger);
    } catch (const std::exception& error) {
        Recorder::Abandon(error.what());
    } catch (...) {
        Recorder::Abandon("unknown error");
    }
}

#endif
