#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

constexpr std::size_t ChunkBytes = 64 * 1024;
constexpr std::size_t MaxChunks = 64;
constexpr std::size_t Align = 16;
constexpr std::size_t Cannot = std::numeric_limits<std::size_t>::max();

constexpr std::size_t padded(std::size_t bytes) {
    return (bytes + Align - 1) & ~(Align - 1);
}

enum class Mode { Inline, Thread };
Mode mode = Mode::Inline;

using Replay = void (*)(const std::byte*);

struct Header {
    Replay replay;
    std::size_t bytes;
};

struct Chunk {
    std::atomic<std::size_t> published{0};
    std::atomic<Chunk*> next{nullptr};
    alignas(Align) std::byte data[ChunkBytes];
};

enum WaitReason : std::size_t { WaitFence, WaitSync, WaitBackpressure, WaitDescriptorPool, WaitPresent, WaitCommandBuffer, WaitOther, WaitReasons };
constexpr std::array<const char*, WaitReasons> WaitNames{"fence or query (a)", "idle or semaphore wait (b)", "chunk pool full (c)", "descriptor or command pool (d)", "present", "command buffer call without the GPU lock", "other"};

struct Stats {
    std::uint64_t entries = 0, bytes = 0, draws = 0, emitNs = 0, flushes = 0;
    std::map<std::string, std::uint64_t> syncs;
    std::int64_t reportedAt = 0;
};

Stats stats;
std::atomic<std::uint64_t> unguardedCommands{0}, unguardedOther{0}, presents{0}, replayNs{0}, notifies{0}, notifyNs{0}, spinUs{100}, failures{0}, verified{0}, mismatches{0};
std::array<std::atomic<std::uint64_t>, WaitReasons> waitCommitterNs{}, waitCommitterCount{}, waitOtherNs{}, waitOtherCount{};
std::uint32_t emitTick = 0;
PFN_vkGetDeviceProcAddr realDeviceProc = nullptr;
const void* emitHolder = nullptr;
bool emitHolderEmits = false;
std::uint64_t clockNs = 0;
std::uint64_t verifyEvery = 0;

std::mutex chunksMutex;
std::vector<Chunk*> freeChunks;
std::size_t allocatedChunks = 0;
std::condition_variable chunkFreed;

Chunk* tail = nullptr;
std::size_t used = 0;
std::atomic<std::uint64_t> emitted{0};
std::atomic<Chunk*> firstChunk{nullptr};
std::atomic<std::uint64_t> publishedTicket{0};

Chunk* head = nullptr;
std::size_t headAt = 0;
std::atomic<std::uint64_t> played{0};
std::uint64_t playedLocal = 0;

std::mutex wakeMutex;
std::condition_variable wakeR;
std::atomic<bool> rSleeping{false};
bool rWake = false;
std::condition_variable drained;
std::atomic<std::uint32_t> drainWaiters{0};

std::mutex queueMutex;
std::mutex rQueuesMutex;
std::unordered_set<VkQueue> rQueues;
std::atomic<bool> lost{false};
std::once_flag rStarted;
std::mutex fencesMutex;
std::unordered_map<VkFence, std::uint32_t> pendingFences;
std::mutex buffersMutex;
std::unordered_set<VkCommandBuffer> streamBuffers;

std::mutex verifyMutex;
std::deque<std::pair<std::uint64_t, std::uint64_t>> verifyQueue;

struct OpCounts {
    const char* name;
    std::uint64_t* entries;
    std::uint64_t* bytes;
};

std::mutex opsMutex;
std::vector<OpCounts> ops;

std::uint64_t elapsed(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

std::uint64_t hashBytes(const std::byte* bytes, std::size_t count) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < count; ++index) hash = (hash ^ static_cast<std::uint8_t>(bytes[index])) * 1099511628211ull;
    return hash;
}

std::string topOps() {
    std::lock_guard lock(opsMutex);
    std::vector<OpCounts> sorted = ops;
    std::sort(sorted.begin(), sorted.end(), [](const OpCounts& left, const OpCounts& right) { return *left.bytes > *right.bytes; });
    std::string line;
    for (std::size_t index = 0; index < sorted.size() && index < 8 && *sorted[index].bytes != 0; ++index) line += " " + std::string(sorted[index].name) + " " + std::to_string(*sorted[index].entries) + " entries " + std::to_string(*sorted[index].bytes) + " B;";
    for (auto& op : ops) *op.entries = *op.bytes = 0;
    return line;
}

std::size_t chunkCount();

void report() {
    const auto tenSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
    if (stats.reportedAt == tenSeconds) return;
    const bool first = stats.reportedAt == 0;
    stats.reportedAt = tenSeconds;
    if (first) return;
    const double draws = stats.draws != 0 ? static_cast<double>(stats.draws) : 1.0;
    std::string syncs;
    for (const auto& [name, count] : stats.syncs) syncs += " " + name + " " + std::to_string(count) + ";";
    const auto byOp = topOps();
    const auto frames = presents.exchange(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[r-inline] 10 s (%s): %llu entries (%.1f per draw/dispatch), %llu bytes (%.0f per draw/dispatch), %llu draws/dispatches; emit %.1f ms (1-of-64 x64, less one clock read of %llu ns each), replay %.1f ms, %llu flushes or publishes, %llu presents, %zu chunks; calls from threads without the GPU lock while entries were pending: %llu command buffer calls (the run is invalid if not 0), %llu others; replayed calls that failed %llu; syncs:%s; top ops by bytes:%s\n", mode == Mode::Thread ? "thread" : "inline", static_cast<unsigned long long>(stats.entries), static_cast<double>(stats.entries) / draws, static_cast<unsigned long long>(stats.bytes), static_cast<double>(stats.bytes) / draws, static_cast<unsigned long long>(stats.draws), static_cast<double>(stats.emitNs) * 64.0 / 1e6, static_cast<unsigned long long>(clockNs), static_cast<double>(replayNs.exchange(0, std::memory_order_relaxed)) / 1e6, static_cast<unsigned long long>(stats.flushes), static_cast<unsigned long long>(frames), chunkCount(), static_cast<unsigned long long>(unguardedCommands.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(unguardedOther.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(failures.exchange(0, std::memory_order_relaxed)), syncs.c_str(), byOp.c_str());
    if (mode == Mode::Thread) {
        std::string waits;
        double committerTotal = 0;
        for (std::size_t reason = 0; reason < WaitReasons; ++reason) {
            const auto committerMs = static_cast<double>(waitCommitterNs[reason].exchange(0, std::memory_order_relaxed)) / 1e6;
            const auto committerCount = waitCommitterCount[reason].exchange(0, std::memory_order_relaxed);
            const auto otherMs = static_cast<double>(waitOtherNs[reason].exchange(0, std::memory_order_relaxed)) / 1e6;
            const auto otherCount = waitOtherCount[reason].exchange(0, std::memory_order_relaxed);
            committerTotal += committerMs;
            if (committerCount + otherCount == 0) continue;
            char part[192];
            std::snprintf(part, sizeof(part), " %s: committer %llu in %.1f ms, other threads %llu in %.1f ms;", WaitNames[reason], static_cast<unsigned long long>(committerCount), committerMs, static_cast<unsigned long long>(otherCount), otherMs);
            waits += part;
        }
        const double perFrame = frames != 0 ? committerTotal / static_cast<double>(frames) : 0.0;
        const auto notified = notifies.exchange(0, std::memory_order_relaxed);
        std::fprintf(stderr, "[r-wait] 10 s: committer waits for R %.1f ms (%.2f ms per frame over %llu presents), %llu real notifies (%.1f per frame, %.1f ms on the notifying threads; R spins %llu us before it sleeps);%s sequence checks %llu, mismatches %llu\n", committerTotal, perFrame, static_cast<unsigned long long>(frames), static_cast<unsigned long long>(notified), frames != 0 ? static_cast<double>(notified) / static_cast<double>(frames) : 0.0, static_cast<double>(notifyNs.exchange(0, std::memory_order_relaxed)) / 1e6, static_cast<unsigned long long>(spinUs.load(std::memory_order_relaxed)), waits.c_str(), static_cast<unsigned long long>(verified.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(mismatches.exchange(0, std::memory_order_relaxed)));
    }
    const auto reportedAt = stats.reportedAt;
    stats = {};
    stats.reportedAt = reportedAt;
}

std::size_t chunkCount() {
    std::lock_guard lock(chunksMutex);
    return allocatedChunks;
}

bool queueOfR(VkQueue queue) {
    std::lock_guard lock(rQueuesMutex);
    return rQueues.contains(queue);
}

bool holder() {
    return GuestMemory::GpuMutex().HeldByThisThread();
}

bool emitting() {
    if (!holder()) return false;
    const auto token = CurrentThreadToken();
    if (token != emitHolder) {
        emitHolder = token;
        emitHolderEmits = mode == Mode::Thread || Recorder::OnCommitter();
    }
    return emitHolderEmits;
}

Chunk* takeChunk() {
    std::unique_lock lock(chunksMutex);
    if (freeChunks.empty() && (mode == Mode::Inline || allocatedChunks < MaxChunks)) {
        ++allocatedChunks;
        return new Chunk;
    }
    if (freeChunks.empty()) {
        const auto start = std::chrono::steady_clock::now();
        chunkFreed.wait(lock, [] { return !freeChunks.empty(); });
        const auto ns = elapsed(start);
        (Recorder::OnCommitter() ? waitCommitterNs : waitOtherNs)[WaitBackpressure].fetch_add(ns, std::memory_order_relaxed);
        (Recorder::OnCommitter() ? waitCommitterCount : waitOtherCount)[WaitBackpressure].fetch_add(1, std::memory_order_relaxed);
    }
    auto* chunk = freeChunks.back();
    freeChunks.pop_back();
    chunk->published.store(0, std::memory_order_relaxed);
    chunk->next.store(nullptr, std::memory_order_relaxed);
    return chunk;
}

void returnChunk(Chunk* chunk) {
    {
        std::lock_guard lock(chunksMutex);
        freeChunks.push_back(chunk);
    }
    chunkFreed.notify_one();
}

void wakeConsumer() {
    if (mode != Mode::Thread || !rSleeping.load(std::memory_order_seq_cst)) return;
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(wakeMutex);
        rWake = true;
    }
    wakeR.notify_one();
    notifies.fetch_add(1, std::memory_order_relaxed);
    notifyNs.fetch_add(elapsed(start), std::memory_order_relaxed);
}

void publish() {
    if (tail == nullptr) return;
    tail->published.store(used, std::memory_order_release);
    const auto ticket = emitted.load(std::memory_order_relaxed);
    if (publishedTicket.load(std::memory_order_relaxed) == ticket) return;
    publishedTicket.store(ticket, std::memory_order_seq_cst);
    ++stats.flushes;
    wakeConsumer();
}

std::byte* reserve(Replay replay, std::size_t bytes) {
    const auto need = sizeof(Header) + padded(bytes);
    if (need > ChunkBytes) return nullptr;
    if (tail == nullptr) {
        tail = takeChunk();
        used = 0;
        firstChunk.store(tail, std::memory_order_release);
    } else if (used + need > ChunkBytes) {
        if (mode == Mode::Thread) publish();
        auto* next = takeChunk();
        tail->published.store(used, std::memory_order_release);
        tail->next.store(next, std::memory_order_release);
        tail = next;
        used = 0;
    }
    auto* at = tail->data + used;
    new (at) Header{replay, padded(bytes)};
    used += need;
    return at + sizeof(Header);
}

void checkSequence(const Header* header) {
    std::uint64_t expected = 0;
    {
        std::lock_guard lock(verifyMutex);
        if (verifyQueue.empty() || verifyQueue.front().first != playedLocal) return;
        expected = verifyQueue.front().second;
        verifyQueue.pop_front();
    }
    verified.fetch_add(1, std::memory_order_relaxed);
    if (hashBytes(reinterpret_cast<const std::byte*>(header + 1), header->bytes) != expected) {
        if (mismatches.fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[r-wait] sequence mismatch at entry %llu\n", static_cast<unsigned long long>(playedLocal));
    }
}

bool consume() {
    if (head == nullptr) {
        head = firstChunk.load(std::memory_order_acquire);
        headAt = 0;
        if (head == nullptr) return false;
    }
    bool progress = false;
    for (;;) {
        const auto end = head->published.load(std::memory_order_acquire);
        if (headAt < end) {
            const auto start = std::chrono::steady_clock::now();
            while (headAt < end) {
                const auto* header = reinterpret_cast<const Header*>(head->data + headAt);
                if (verifyEvery != 0) checkSequence(header);
                header->replay(head->data + headAt + sizeof(Header));
                headAt += sizeof(Header) + header->bytes;
                ++playedLocal;
            }
            replayNs.fetch_add(elapsed(start), std::memory_order_relaxed);
            played.store(playedLocal, std::memory_order_seq_cst);
            progress = true;
            if (drainWaiters.load(std::memory_order_seq_cst) != 0) {
                std::lock_guard lock(wakeMutex);
                drained.notify_all();
            }
            continue;
        }
        auto* next = head->next.load(std::memory_order_acquire);
        if (next == nullptr) return progress;
        if (headAt < head->published.load(std::memory_order_acquire)) continue;
        auto* done = head;
        head = next;
        headAt = 0;
        if (mode == Mode::Thread) returnChunk(done);
        else {
            std::lock_guard lock(chunksMutex);
            freeChunks.push_back(done);
        }
    }
}

void flush(const char* reason) {
    if (mode != Mode::Inline || played.load(std::memory_order_relaxed) == emitted.load(std::memory_order_relaxed)) return;
    publish();
    consume();
    ++stats.syncs[reason];
    report();
}

void noteWait(WaitReason reason, std::uint64_t ns) {
    const bool committer = Recorder::OnCommitter();
    (committer ? waitCommitterNs : waitOtherNs)[reason].fetch_add(ns, std::memory_order_relaxed);
    (committer ? waitCommitterCount : waitOtherCount)[reason].fetch_add(1, std::memory_order_relaxed);
}

void drainWait(WaitReason reason, const char* name) {
    if (lost.load(std::memory_order_acquire)) return;
    if (holder()) publish();
    const auto target = publishedTicket.load(std::memory_order_acquire);
    if (played.load(std::memory_order_acquire) >= target) return;
    const auto start = std::chrono::steady_clock::now();
    for (int spin = 0; spin < 2000 && played.load(std::memory_order_acquire) < target; ++spin) std::this_thread::yield();
    if (played.load(std::memory_order_acquire) < target) {
        drainWaiters.fetch_add(1, std::memory_order_seq_cst);
        std::unique_lock lock(wakeMutex);
        while (played.load(std::memory_order_seq_cst) < target && !lost.load(std::memory_order_acquire)) drained.wait_for(lock, std::chrono::milliseconds(1));
        drainWaiters.fetch_sub(1, std::memory_order_seq_cst);
    }
    noteWait(reason, elapsed(start));
    if (holder()) {
        ++stats.syncs[name];
        report();
    }
}

void syncFor(const char* name, WaitReason reason) {
    if (mode == Mode::Thread) {
        if (played.load(std::memory_order_acquire) != emitted.load(std::memory_order_relaxed)) drainWait(reason, name);
        return;
    }
    if (emitted.load(std::memory_order_relaxed) == played.load(std::memory_order_acquire)) return;
    if (holder()) {
        flush(name);
        return;
    }
    const bool command = std::strncmp(name, "vkCmd", 5) == 0 || std::strstr(name, "CommandBuffer") != nullptr || std::strstr(name, "CommandPool") != nullptr || std::strncmp(name, "vkQueueSubmit", 13) == 0;
    (command ? unguardedCommands : unguardedOther).fetch_add(1, std::memory_order_relaxed);
}

void releaseHook() {
    emitHolder = nullptr;
    if (mode == Mode::Thread) {
        publish();
        report();
    } else {
        flush("GPU lock release");
    }
}

void markLost(const char* name, int result) {
    lost.store(true, std::memory_order_release);
    if (failures.fetch_add(1, std::memory_order_relaxed) < 4) std::fprintf(stderr, "[r-wait] replayed %s failed: %d; waits now report a lost device\n", name, result);
}

void runR() {
    if (CpuTopology::ReservedCore() != 0) CpuTopology::PinTraced("draw recorder R", nullptr, CpuTopology::Get().process);
    for (;;) {
        if (consume()) continue;
        bool found = false;
        const auto spinStart = std::chrono::steady_clock::now();
        while (!found && std::chrono::steady_clock::now() - spinStart < std::chrono::microseconds(spinUs.load(std::memory_order_relaxed))) {
            found = played.load(std::memory_order_relaxed) < publishedTicket.load(std::memory_order_acquire);
        }
        if (found) continue;
        rSleeping.store(true, std::memory_order_seq_cst);
        if (played.load(std::memory_order_relaxed) < publishedTicket.load(std::memory_order_seq_cst)) {
            rSleeping.store(false, std::memory_order_relaxed);
            continue;
        }
        std::unique_lock lock(wakeMutex);
        wakeR.wait_for(lock, std::chrono::milliseconds(50), [] { return rWake; });
        rWake = false;
        rSleeping.store(false, std::memory_order_relaxed);
    }
}

int descriptorKind(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return 0;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return 1;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return 2;
    default:
        return -1;
    }
}

struct Writer {
    std::byte* at;

    template<typename T>
    const T* copy(const T* source, std::size_t count) {
        if (source == nullptr || count == 0) return source;
        auto* target = reinterpret_cast<T*>(at);
        std::memcpy(target, source, count * sizeof(T));
        at += padded(count * sizeof(T));
        return target;
    }

    const void* bytes(const void* source, std::size_t count) {
        if (source == nullptr || count == 0) return source;
        std::memcpy(at, source, count);
        auto* target = at;
        at += padded(count);
        return target;
    }
};

template<typename T>
std::size_t arrayBytes(const T* items, std::size_t count) {
    return items == nullptr ? 0 : padded(count * sizeof(T));
}

template<typename T>
bool chained(const T* items, std::size_t count) {
    if constexpr (requires(const T& item) { item.pNext; }) {
        for (std::size_t index = 0; items != nullptr && index < count; ++index) {
            if (items[index].pNext != nullptr) return true;
        }
    }
    return false;
}

template<std::size_t P, std::size_t N>
struct ArrayAt {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto* items = std::get<P>(args);
        const auto count = static_cast<std::size_t>(std::get<N>(args));
        if (chained(items, count)) return Cannot;
        return arrayBytes(items, count);
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        std::get<P>(args) = writer.copy(std::get<P>(args), static_cast<std::size_t>(std::get<N>(args)));
    }
};

template<std::size_t P>
struct OneAt {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        if (chained(std::get<P>(args), 1)) return Cannot;
        return arrayBytes(std::get<P>(args), 1);
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        std::get<P>(args) = writer.copy(std::get<P>(args), 1);
    }
};

template<std::size_t P, std::size_t N>
struct BytesAt {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        return std::get<P>(args) == nullptr ? 0 : padded(static_cast<std::size_t>(std::get<N>(args)));
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        std::get<P>(args) = writer.bytes(std::get<P>(args), static_cast<std::size_t>(std::get<N>(args)));
    }
};

struct ByValue {
    template<typename Args>
    static std::size_t Size(const Args&) { return 0; }
    template<typename Args>
    static void Copy(Args&, Writer&) {}
};

template<typename... Parts>
struct Deep {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        std::size_t total = 0;
        for (const auto part : {Parts::Size(args)...}) {
            if (part == Cannot) return Cannot;
            total += part;
        }
        return total;
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        (Parts::Copy(args, writer), ...);
    }
};

struct DescriptorWrites {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto count = std::get<1>(args);
        const auto* writes = std::get<2>(args);
        std::size_t total = arrayBytes(writes, count) + arrayBytes(std::get<4>(args), std::get<3>(args));
        if (chained(writes, count) || chained(std::get<4>(args), std::get<3>(args))) return Cannot;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& write = writes[index];
            const auto kind = descriptorKind(write.descriptorType);
            if (kind < 0) return Cannot;
            total += kind == 0 ? padded(write.descriptorCount * sizeof(VkDescriptorImageInfo)) : kind == 1 ? padded(write.descriptorCount * sizeof(VkDescriptorBufferInfo)) : padded(write.descriptorCount * sizeof(VkBufferView));
        }
        return total;
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        auto* writes = const_cast<VkWriteDescriptorSet*>(writer.copy(std::get<2>(args), std::get<1>(args)));
        std::get<2>(args) = writes;
        std::get<4>(args) = writer.copy(std::get<4>(args), std::get<3>(args));
        for (std::uint32_t index = 0; writes != nullptr && index < std::get<1>(args); ++index) {
            auto& write = writes[index];
            const auto kind = descriptorKind(write.descriptorType);
            write.pImageInfo = kind == 0 ? writer.copy(write.pImageInfo, write.descriptorCount) : nullptr;
            write.pBufferInfo = kind == 1 ? writer.copy(write.pBufferInfo, write.descriptorCount) : nullptr;
            write.pTexelBufferView = kind == 2 ? writer.copy(write.pTexelBufferView, write.descriptorCount) : nullptr;
        }
    }
};

struct SubmitInfos {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto count = std::get<1>(args);
        const auto* submits = std::get<2>(args);
        std::size_t total = arrayBytes(submits, count);
        for (std::uint32_t index = 0; submits != nullptr && index < count; ++index) {
            const auto& submit = submits[index];
            if (submit.pNext != nullptr) {
                const auto* timeline = static_cast<const VkTimelineSemaphoreSubmitInfo*>(submit.pNext);
                if (timeline->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO || timeline->pNext != nullptr) return Cannot;
                total += padded(sizeof(VkTimelineSemaphoreSubmitInfo)) + arrayBytes(timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount) + arrayBytes(timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount);
            }
            total += arrayBytes(submit.pWaitSemaphores, submit.waitSemaphoreCount) + arrayBytes(submit.pWaitDstStageMask, submit.waitSemaphoreCount) + arrayBytes(submit.pCommandBuffers, submit.commandBufferCount) + arrayBytes(submit.pSignalSemaphores, submit.signalSemaphoreCount);
        }
        return total;
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        auto* submits = const_cast<VkSubmitInfo*>(writer.copy(std::get<2>(args), std::get<1>(args)));
        std::get<2>(args) = submits;
        for (std::uint32_t index = 0; submits != nullptr && index < std::get<1>(args); ++index) {
            auto& submit = submits[index];
            if (submit.pNext != nullptr) {
                auto* timeline = const_cast<VkTimelineSemaphoreSubmitInfo*>(writer.copy(static_cast<const VkTimelineSemaphoreSubmitInfo*>(submit.pNext), 1));
                timeline->pWaitSemaphoreValues = writer.copy(timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount);
                timeline->pSignalSemaphoreValues = writer.copy(timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount);
                submit.pNext = timeline;
            }
            submit.pWaitSemaphores = writer.copy(submit.pWaitSemaphores, submit.waitSemaphoreCount);
            submit.pWaitDstStageMask = writer.copy(submit.pWaitDstStageMask, submit.waitSemaphoreCount);
            submit.pCommandBuffers = writer.copy(submit.pCommandBuffers, submit.commandBufferCount);
            submit.pSignalSemaphores = writer.copy(submit.pSignalSemaphores, submit.signalSemaphoreCount);
        }
    }
};

struct BeginInfo {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto* info = std::get<1>(args);
        if (info == nullptr || info->pNext != nullptr) return Cannot;
        if (info->pInheritanceInfo != nullptr && info->pInheritanceInfo->pNext != nullptr) return Cannot;
        return padded(sizeof(VkCommandBufferBeginInfo)) + arrayBytes(info->pInheritanceInfo, 1);
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        auto* info = const_cast<VkCommandBufferBeginInfo*>(writer.copy(std::get<1>(args), 1));
        info->pInheritanceInfo = writer.copy(info->pInheritanceInfo, 1);
        std::get<1>(args) = info;
    }
};

struct RenderingInfo {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto* info = std::get<1>(args);
        if (info == nullptr || info->pNext != nullptr || chained(info->pColorAttachments, info->colorAttachmentCount) || chained(info->pDepthAttachment, 1) || chained(info->pStencilAttachment, 1)) return Cannot;
        return padded(sizeof(VkRenderingInfo)) + arrayBytes(info->pColorAttachments, info->colorAttachmentCount) + arrayBytes(info->pDepthAttachment, 1) + arrayBytes(info->pStencilAttachment, 1);
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        auto* info = const_cast<VkRenderingInfo*>(writer.copy(std::get<1>(args), 1));
        info->pColorAttachments = writer.copy(info->pColorAttachments, info->colorAttachmentCount);
        info->pDepthAttachment = writer.copy(info->pDepthAttachment, 1);
        info->pStencilAttachment = writer.copy(info->pStencilAttachment, 1);
        std::get<1>(args) = info;
    }
};

struct RenderPassBegin {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto* info = std::get<1>(args);
        if (info == nullptr || info->pNext != nullptr) return Cannot;
        return padded(sizeof(VkRenderPassBeginInfo)) + arrayBytes(info->pClearValues, info->clearValueCount);
    }
    template<typename Args>
    static void Copy(Args& args, Writer& writer) {
        auto* info = const_cast<VkRenderPassBeginInfo*>(writer.copy(std::get<1>(args), 1));
        info->pClearValues = writer.copy(info->pClearValues, info->clearValueCount);
        std::get<1>(args) = info;
    }
};

struct Sync {};

template<typename Copier>
struct ThreadOnly {};

template<typename Copier>
struct Unwrap {
    using Type = Copier;
    static constexpr bool threadOnly = false;
};

template<typename Copier>
struct Unwrap<ThreadOnly<Copier>> {
    using Type = Copier;
    static constexpr bool threadOnly = true;
};

template<typename Args>
VkCommandBuffer commandBufferOf(const Args& args) {
    if constexpr (std::tuple_size_v<Args> != 0 && std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, Args>>, VkCommandBuffer>) return std::get<0>(args);
    return VK_NULL_HANDLE;
}

bool streamBuffer(VkCommandBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) return false;
    std::lock_guard lock(buffersMutex);
    return streamBuffers.contains(buffer);
}

WaitReason reasonOf(const char* name) {
    if (std::strstr(name, "DescriptorPool") != nullptr || std::strstr(name, "DescriptorSets") != nullptr || std::strstr(name, "CommandBuffers") != nullptr) return WaitDescriptorPool;
    if (std::strstr(name, "Fence") != nullptr || std::strstr(name, "Query") != nullptr) return WaitFence;
    if (std::strstr(name, "Idle") != nullptr || std::strstr(name, "Semaphore") != nullptr) return WaitSync;
    return WaitOther;
}

template<std::size_t Slot, typename F, typename Kind>
struct Deferred;

template<std::size_t Slot, typename Kind, typename R, typename... A>
struct Deferred<Slot, R(VKAPI_PTR*)(A...), Kind> {
    using Args = std::tuple<A...>;
    using Copier = typename Unwrap<Kind>::Type;
    static constexpr bool threadOnly = Unwrap<Kind>::threadOnly;
    static constexpr bool submit = std::is_same_v<Args, std::tuple<VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence>>;
    static constexpr bool begin = std::is_same_v<Args, std::tuple<VkCommandBuffer, const VkCommandBufferBeginInfo*>>;
    static constexpr bool freeBuffers = std::is_same_v<Args, std::tuple<VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*>>;
    static inline R(VKAPI_PTR* real)(A...) = nullptr;
    static inline const char* name = nullptr;
    static inline std::string refused;
    static inline WaitReason reason = WaitOther;
    static inline bool draw = false;
    static inline std::uint64_t opEntries = 0;
    static inline std::uint64_t opBytes = 0;

    static void Play(const std::byte* payload) {
        const auto& args = *reinterpret_cast<const Args*>(payload);
        if constexpr (submit) {
            VkResult result;
            {
                std::lock_guard lock(queueMutex);
                result = std::apply(real, args);
            }
            if (result != VK_SUCCESS) markLost(name, static_cast<int>(result));
            if (const auto fence = std::get<3>(args); fence != VK_NULL_HANDLE) {
                std::lock_guard lock(fencesMutex);
                if (const auto found = pendingFences.find(fence); found != pendingFences.end() && --found->second == 0) pendingFences.erase(found);
            }
        } else if constexpr (std::is_same_v<R, void>) {
            std::apply(real, args);
        } else {
            const auto result = std::apply(real, args);
            if constexpr (std::is_same_v<R, VkResult>) {
                if (result != VK_SUCCESS) markLost(name, static_cast<int>(result));
            }
        }
    }

    static R direct(A... arguments) {
        if constexpr (sizeof...(A) != 0 && std::is_same_v<std::tuple_element_t<0, Args>, VkQueue>) {
            std::unique_lock lock(queueMutex, std::defer_lock);
            if (queueOfR(std::get<0>(std::forward_as_tuple(arguments...)))) lock.lock();
            return real(arguments...);
        } else {
            return real(arguments...);
        }
    }

    static R VKAPI_CALL Call(A... arguments) {
        if constexpr (std::is_same_v<Copier, Sync>) {
            syncFor(name, reason);
            return direct(arguments...);
        } else {
            if ((threadOnly && mode != Mode::Thread) || !emitting()) {
                if (mode != Mode::Thread) {
                    syncFor(name, reason);
                } else if (streamBuffer(commandBufferOf(std::forward_as_tuple(arguments...)))) {
                    unguardedCommands.fetch_add(1, std::memory_order_relaxed);
                    drainWait(WaitCommandBuffer, name);
                } else if (played.load(std::memory_order_acquire) != emitted.load(std::memory_order_relaxed)) {
                    unguardedOther.fetch_add(1, std::memory_order_relaxed);
                }
                return direct(arguments...);
            }
            const bool sampled = (emitTick++ & 63u) == 32u;
            const auto start = sampled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto more = Copier::Size(std::forward_as_tuple(arguments...));
            std::byte* at = more == Cannot ? nullptr : reserve(&Play, padded(sizeof(Args)) + more);
            if (at == nullptr) {
                if (mode == Mode::Thread) drainWait(reasonOf(name), refused.c_str());
                else flush(refused.c_str());
                return direct(arguments...);
            }
            auto* stored = new (at) Args(arguments...);
            Writer writer{at + padded(sizeof(Args))};
            Copier::Copy(*stored, writer);
            const auto index = emitted.load(std::memory_order_relaxed);
            if (verifyEvery != 0 && index % verifyEvery == 0) {
                std::lock_guard lock(verifyMutex);
                verifyQueue.emplace_back(index, hashBytes(at, padded(sizeof(Args)) + more));
            }
            emitted.store(index + 1, std::memory_order_relaxed);
            if constexpr (submit) {
                if (const auto fence = std::get<3>(*stored); fence != VK_NULL_HANDLE) {
                    std::lock_guard lock(fencesMutex);
                    ++pendingFences[fence];
                }
            }
            if constexpr (begin) {
                std::lock_guard lock(buffersMutex);
                streamBuffers.insert(std::get<0>(*stored));
            }
            if constexpr (freeBuffers) {
                std::lock_guard lock(buffersMutex);
                for (std::uint32_t index = 0; std::get<3>(*stored) != nullptr && index < std::get<2>(*stored); ++index) streamBuffers.erase(std::get<3>(*stored)[index]);
            }
            if constexpr (submit) {
                std::lock_guard lock(rQueuesMutex);
                rQueues.insert(std::get<0>(*stored));
            }
            const auto bytes = sizeof(Header) + padded(sizeof(Args)) + more;
            ++stats.entries;
            stats.bytes += bytes;
            ++opEntries;
            opBytes += bytes;
            if (draw) ++stats.draws;
            if (sampled) {
                const auto ns = elapsed(start);
                stats.emitNs += ns > clockNs ? ns - clockNs : 0;
            }
            if constexpr (!std::is_same_v<R, void>) return R{};
        }
    }
};

template<std::size_t Slot, typename F, typename Kind>
PFN_vkVoidFunction wrap(PFN_vkVoidFunction real, const char* name) {
    using Wrapper = Deferred<Slot, F, Kind>;
    Wrapper::real = reinterpret_cast<F>(real);
    Wrapper::name = name;
    Wrapper::reason = reasonOf(name);
    if (Wrapper::refused.empty()) Wrapper::refused = std::string("refused:") + name;
    Wrapper::draw = std::strncmp(name, "vkCmdDraw", 9) == 0 || std::strncmp(name, "vkCmdDispatch", 13) == 0;
    static std::once_flag registered;
    std::call_once(registered, [name] {
        std::lock_guard lock(opsMutex);
        ops.push_back({name, &Wrapper::opEntries, &Wrapper::opBytes});
    });
    return reinterpret_cast<PFN_vkVoidFunction>(&Wrapper::Call);
}

PFN_vkGetFenceStatus realFenceStatus = nullptr;
PFN_vkWaitForFences realWaitForFences = nullptr;
PFN_vkQueuePresentKHR realPresent = nullptr;
PFN_vkQueueWaitIdle realQueueWaitIdle = nullptr;
PFN_vkDeviceWaitIdle realDeviceWaitIdle = nullptr;

bool fencePending(VkFence fence) {
    std::lock_guard lock(fencesMutex);
    return pendingFences.contains(fence);
}

VkResult VKAPI_CALL fenceStatus(VkDevice device, VkFence fence) {
    if (lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
    if (mode == Mode::Thread) {
        if (fencePending(fence)) {
            if (holder()) publish();
            return VK_NOT_READY;
        }
    } else {
        syncFor("vkGetFenceStatus", WaitFence);
    }
    return realFenceStatus(device, fence);
}

VkResult VKAPI_CALL waitForFences(VkDevice device, std::uint32_t count, const VkFence* fences, VkBool32 all, std::uint64_t timeout) {
    if (mode == Mode::Thread) {
        const auto pendingAny = [&] {
            for (std::uint32_t index = 0; index < count; ++index) {
                if (fencePending(fences[index])) return true;
            }
            return false;
        };
        if (pendingAny()) {
            drainWait(WaitFence, "vkWaitForFences");
            const auto start = std::chrono::steady_clock::now();
            bool waited = false;
            while (pendingAny() && !lost.load(std::memory_order_acquire)) {
                waited = true;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (waited) noteWait(WaitFence, elapsed(start));
        }
        if (lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
    } else {
        syncFor("vkWaitForFences", WaitFence);
    }
    return realWaitForFences(device, count, fences, all, timeout);
}

VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR* info) {
    if (mode == Mode::Thread) drainWait(WaitPresent, "vkQueuePresentKHR");
    else syncFor("vkQueuePresentKHR", WaitPresent);
    presents.fetch_add(1, std::memory_order_relaxed);
    if (lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
    std::unique_lock lock(queueMutex, std::defer_lock);
    if (queueOfR(queue)) lock.lock();
    return realPresent(queue, info);
}

VkResult VKAPI_CALL queueWaitIdle(VkQueue queue) {
    if (mode == Mode::Thread) drainWait(WaitSync, "vkQueueWaitIdle");
    else syncFor("vkQueueWaitIdle", WaitSync);
    std::unique_lock lock(queueMutex, std::defer_lock);
    if (queueOfR(queue)) lock.lock();
    return realQueueWaitIdle(queue);
}

VkResult VKAPI_CALL deviceWaitIdle(VkDevice device) {
    if (mode == Mode::Thread) drainWait(WaitSync, "vkDeviceWaitIdle");
    else syncFor("vkDeviceWaitIdle", WaitSync);
    std::lock_guard lock(queueMutex);
    return realDeviceWaitIdle(device);
}

template<typename F>
PFN_vkVoidFunction special(PFN_vkVoidFunction real, F& target, F wrapper) {
    target = reinterpret_cast<F>(real);
    return reinterpret_cast<PFN_vkVoidFunction>(wrapper);
}

PFN_vkVoidFunction VKAPI_CALL inlineDeviceProc(VkDevice device, const char* name) {
    const auto real = realDeviceProc(device, name);
    if (real == nullptr) return real;
#define X(n, ...) \
    if (std::strcmp(name, #n) == 0) return wrap<__COUNTER__, PFN_##n, __VA_ARGS__>(real, #n);
    X(vkCmdBeginQuery, ByValue)
    X(vkCmdBeginRenderPass, ThreadOnly<RenderPassBegin>)
    X(vkCmdBeginRenderingKHR, ThreadOnly<RenderingInfo>)
    X(vkCmdBindDescriptorSets, Deep<ArrayAt<5, 4>, ArrayAt<7, 6>>)
    X(vkCmdBindIndexBuffer, ByValue)
    X(vkCmdBindPipeline, ByValue)
    X(vkCmdBindVertexBuffers, Deep<ArrayAt<3, 2>, ArrayAt<4, 2>>)
    X(vkCmdBlitImage, Deep<ArrayAt<6, 5>>)
    X(vkCmdClearColorImage, Deep<OneAt<3>, ArrayAt<5, 4>>)
    X(vkCmdClearDepthStencilImage, Deep<OneAt<3>, ArrayAt<5, 4>>)
    X(vkCmdCopyBuffer, Deep<ArrayAt<4, 3>>)
    X(vkCmdCopyBufferToImage, Deep<ArrayAt<5, 4>>)
    X(vkCmdCopyImage, Deep<ArrayAt<6, 5>>)
    X(vkCmdCopyImageToBuffer, Deep<ArrayAt<5, 4>>)
    X(vkCmdCopyQueryPoolResults, ByValue)
    X(vkCmdDispatch, ByValue)
    X(vkCmdDispatchIndirect, ByValue)
    X(vkCmdDraw, ByValue)
    X(vkCmdDrawIndexed, ByValue)
    X(vkCmdDrawIndexedIndirect, ByValue)
    X(vkCmdDrawIndexedIndirectCountKHR, ByValue)
    X(vkCmdDrawIndirect, ByValue)
    X(vkCmdDrawIndirectCountKHR, ByValue)
    X(vkCmdDrawMeshTasksEXT, ByValue)
    X(vkCmdDrawMeshTasksIndirectEXT, ByValue)
    X(vkCmdEndQuery, ByValue)
    X(vkCmdEndRenderPass, ByValue)
    X(vkCmdEndRenderingKHR, ByValue)
    X(vkCmdFillBuffer, ByValue)
    X(vkCmdPipelineBarrier, Deep<ArrayAt<5, 4>, ArrayAt<7, 6>, ArrayAt<9, 8>>)
    X(vkCmdPushConstants, Deep<BytesAt<5, 4>>)
    X(vkCmdResetQueryPool, ByValue)
    X(vkCmdResolveImage, Deep<ArrayAt<6, 5>>)
    X(vkCmdSetCheckpointNV, ByValue)
    X(vkCmdSetCullModeEXT, ByValue)
    X(vkCmdSetDepthBias, ByValue)
    X(vkCmdSetDepthBounds, ByValue)
    X(vkCmdSetDepthBoundsTestEnableEXT, ByValue)
    X(vkCmdSetDepthCompareOpEXT, ByValue)
    X(vkCmdSetDepthTestEnableEXT, ByValue)
    X(vkCmdSetDepthWriteEnableEXT, ByValue)
    X(vkCmdSetFrontFaceEXT, ByValue)
    X(vkCmdSetScissor, Deep<ArrayAt<3, 2>>)
    X(vkCmdSetStencilCompareMask, ByValue)
    X(vkCmdSetStencilOpEXT, ByValue)
    X(vkCmdSetStencilReference, ByValue)
    X(vkCmdSetStencilTestEnableEXT, ByValue)
    X(vkCmdSetStencilWriteMask, ByValue)
    X(vkCmdSetViewport, Deep<ArrayAt<3, 2>>)
    X(vkCmdUpdateBuffer, Deep<BytesAt<4, 3>>)
    X(vkCmdWriteBufferMarkerAMD, ByValue)
    X(vkCmdWriteTimestamp, ByValue)
    X(vkUpdateDescriptorSets, DescriptorWrites)
    X(vkBeginCommandBuffer, ThreadOnly<BeginInfo>)
    X(vkEndCommandBuffer, ThreadOnly<ByValue>)
    X(vkResetCommandBuffer, ThreadOnly<ByValue>)
    X(vkResetCommandPool, ThreadOnly<ByValue>)
    X(vkFreeCommandBuffers, ThreadOnly<Deep<ArrayAt<3, 2>>>)
    X(vkQueueSubmit, ThreadOnly<SubmitInfos>)
    X(vkDestroyBuffer, ThreadOnly<ByValue>)
    X(vkDestroyBufferView, ThreadOnly<ByValue>)
    X(vkDestroyImage, ThreadOnly<ByValue>)
    X(vkDestroyImageView, ThreadOnly<ByValue>)
    X(vkDestroySampler, ThreadOnly<ByValue>)
    X(vkDestroyPipeline, ThreadOnly<ByValue>)
    X(vkDestroyPipelineLayout, ThreadOnly<ByValue>)
    X(vkDestroyRenderPass, ThreadOnly<ByValue>)
    X(vkDestroyFramebuffer, ThreadOnly<ByValue>)
    X(vkDestroyQueryPool, ThreadOnly<ByValue>)
    X(vkDestroyCommandPool, ThreadOnly<ByValue>)
    X(vkQueueSubmit2KHR, Sync)
    X(vkQueueBindSparse, Sync)
    X(vkDestroyFence, ThreadOnly<ByValue>)
    X(vkDestroySemaphore, ThreadOnly<ByValue>)
    X(vkDestroyDescriptorSetLayout, ThreadOnly<ByValue>)
    X(vkResetFences, Sync)
    X(vkGetQueryPoolResults, Sync)
    X(vkWaitSemaphoresKHR, Sync)
    X(vkGetSemaphoreCounterValueKHR, Sync)
    X(vkResetDescriptorPool, Sync)
    X(vkFreeDescriptorSets, Sync)
    X(vkDestroyDescriptorPool, Sync)
    X(vkAllocateCommandBuffers, Sync)
    X(vkFreeMemory, Sync)
    X(vkUnmapMemory, Sync)
    X(vkDestroyDevice, Sync)
#undef X
    if (std::strcmp(name, "vkGetFenceStatus") == 0) return special(real, realFenceStatus, &fenceStatus);
    if (std::strcmp(name, "vkWaitForFences") == 0) return special(real, realWaitForFences, &waitForFences);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) return special(real, realPresent, &present);
    if (std::strcmp(name, "vkQueueWaitIdle") == 0) return special(real, realQueueWaitIdle, &queueWaitIdle);
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0) return special(real, realDeviceWaitIdle, &deviceWaitIdle);
    return real;
}

}

PFN_vkGetDeviceProcAddr InlineDeviceProc(PFN_vkGetDeviceProcAddr real) {
    const bool threaded = std::getenv("APS5_R_THREAD") != nullptr;
    if (std::getenv("APS5_R_INLINE") == nullptr && !threaded) return real;
    mode = threaded ? Mode::Thread : Mode::Inline;
    lost.store(false, std::memory_order_release);
    realDeviceProc = real;
    if (const char* every = std::getenv("APS5_R_VERIFY"); every != nullptr) verifyEvery = std::strtoull(every, nullptr, 10);
    if (const char* spin = std::getenv("APS5_R_SPIN_US"); spin != nullptr) spinUs.store(std::strtoull(spin, nullptr, 10), std::memory_order_relaxed);
    constexpr int reads = 4096;
    const auto first = std::chrono::steady_clock::now();
    auto last = first;
    for (int read = 0; read < reads; ++read) last = std::chrono::steady_clock::now();
    clockNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count()) / reads;
    GuestMemory::SetGpuReleaseHook(&releaseHook);
    if (threaded) std::call_once(rStarted, [] { std::thread(runR).detach(); });
    std::fprintf(stderr, "[r-inline] on (%s): the Vulkan calls made under the GPU lock go into a stream%s; sequence check every %llu entries\n", threaded ? "thread" : "inline", threaded ? " that the recording thread R replays" : " that the committer replays at the next sync point", static_cast<unsigned long long>(verifyEvery));
    return &inlineDeviceProc;
}

}
