#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/KytyCapture.hpp"
#include "prx/libc/include/KytyGpu.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
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

thread_local std::uint64_t t_callOrdinal = 0;

struct Range {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::uint32_t protection = 0;
    bool writable = false;
    bool donated = false;
};

bool IsSubmit(const char* nid) {
    static constexpr const char* submits[] = {"UglJIZjGssM", "AhGvpITrf4M", "6UzEidRZwkg", "+T8Xo6LtFJI", "b4fpgH5ZXxQ", "Fj7r9EHzF38", "gSRnr79F8tQ", "HF3YllT3mXU", "U46NwOiJpys"};
    return std::any_of(std::begin(submits), std::end(submits), [nid](const char* submit) { return std::strcmp(nid, submit) == 0; });
}

std::uint64_t Hash(const std::byte* bytes, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto* words = reinterpret_cast<const std::uint64_t*>(bytes);
    for (std::size_t i = 0; i < size / 8; ++i) hash = (hash ^ words[i]) * 0x100000001b3ull;
    return hash;
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
        if (state == State::Done || starting) return;
        const auto flips = flipCount != nullptr ? flipCount() : 0;
        if (flips != lastFlips) {
            lastFlips = flips;
            if (boundary()) {
                starting = true;
                lock.unlock();
                waitIdle();
                lock.lock();
                starting = false;
                start();
            }
        }
        if (state != State::Recording) return;
        if (IsSubmit(nid)) submitDelta();
        record(nid, registers);
    }

    void Callback(std::uint64_t function, std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t result) {
        if (abandoned.load()) return;
        std::lock_guard lock(mutex);
        if (state != State::Recording) return;
        FrameCall call;
        call.kind = FrameCallKind::GuestCallback;
        call.ordinal = ++ordinal;
        call.timeNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - origin).count());
        call.thread = GetCurrentThreadId();
        call.values = {function, first, second, third, result, 0, t_callOrdinal, call.thread};
        const auto* object = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(first));
        call.values.insert(call.values.end(), object, object + CallbackObjectWords);
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
        std::error_code error;
        return trigger.empty() || std::filesystem::exists(trigger, error);
    }

    void start() {
        const auto began = std::chrono::steady_clock::now();
        writer = std::make_unique<FrameCaptureWriter>(directory);
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
        std::fprintf(stderr, "[kyty-capture] started: %zu ranges, %.1f MiB stored in %.1f s\n", ranges.size(), static_cast<double>(snapshotBytes) / (1024.0 * 1024.0), std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
    }

    void hashPages(std::uint64_t address, std::span<const std::byte> bytes, bool emit) {
        for (std::size_t at = 0; at + PageBytes <= bytes.size(); at += PageBytes) {
            const auto hash = Hash(bytes.data() + at, PageBytes);
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
                const auto written = KytyGpuCaptureTakeWritten_nid_no_patch(static_cast<std::uintptr_t>(range.address), static_cast<std::size_t>(range.bytes));
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

    void record(const char* nid, const KytyCaptureRegisters& registers) {
        FrameCall call;
        call.kind = FrameCallKind::GuestCall;
        call.ordinal = ++ordinal;
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
        MEMORY_BASIC_INFORMATION info{};
        std::uint64_t stackBytes = 0;
        if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(call.address)), &info, sizeof(info)) != 0) stackBytes = std::min<std::uint64_t>(StackBytes, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize - call.address) & ~std::uint64_t{7};
        call.size = stackBytes;
        const auto* stack = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(call.address));
        call.values.insert(call.values.end(), stack, stack + stackBytes / 8);
        writer->WriteCall(call);
        t_callOrdinal = call.ordinal;
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
        std::snprintf(header, sizeof(header), "{\"kind\":\"kyty-guest-calls\",\"arena_base\":%llu,\"frames\":%llu,\"calls\":%llu,\"callbacks\":%llu,\"ranges\":%zu,\"snapshot_bytes\":%llu,\"delta_bytes\":%llu,\"stack_bytes\":%llu}", static_cast<unsigned long long>(arenaBase), static_cast<unsigned long long>(recordedFrames), static_cast<unsigned long long>(recordedCalls), static_cast<unsigned long long>(recordedCallbacks), ranges.size(), static_cast<unsigned long long>(snapshotBytes), static_cast<unsigned long long>(deltaBytes), static_cast<unsigned long long>(StackBytes));
        writer->Finish(header);
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

extern "C" void AgcDriverKytyCaptureCallback_nid_no_patch(std::uint64_t function, std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t result) {
    if (Recorder::Abandoned()) return;
    try {
        if (auto* recorder = Recorder::Active()) recorder->Callback(function, first, second, third, result);
    } catch (const std::exception& error) {
        Recorder::Abandon(error.what());
    } catch (...) {
        Recorder::Abandon("unknown error");
    }
}

#endif
