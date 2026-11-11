#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

constexpr std::size_t ChunkBytes = 64 * 1024;
constexpr std::size_t Align = 16;
constexpr std::size_t Cannot = std::numeric_limits<std::size_t>::max();

constexpr std::size_t padded(std::size_t bytes) {
    return (bytes + Align - 1) & ~(Align - 1);
}

using Replay = void (*)(const std::byte*);

struct Header {
    Replay replay;
    std::size_t bytes;
};

struct Stats {
    std::uint64_t entries = 0, bytes = 0, draws = 0, emitNs = 0, emitSampled = 0, replayNs = 0, flushes = 0;
    std::map<std::string, std::uint64_t> syncs;
    std::int64_t reportedAt = 0;
};

class Stream {
public:
    bool Empty() const { return used == 0 && full.empty(); }

    std::byte* Reserve(Replay replay, std::size_t bytes) {
        const auto need = sizeof(Header) + padded(bytes);
        if (need > ChunkBytes) return nullptr;
        if (current == nullptr || used + need > ChunkBytes) {
            if (current != nullptr) full.emplace_back(std::move(current), used);
            current = take();
            used = 0;
        }
        auto* at = current.get() + used;
        new (at) Header{replay, padded(bytes)};
        used += need;
        return at + sizeof(Header);
    }

    void Play() {
        for (auto& [chunk, end] : full) play(chunk.get(), end);
        if (current != nullptr) play(current.get(), used);
        for (auto& [chunk, end] : full) pool.push_back(std::move(chunk));
        full.clear();
        used = 0;
    }

    std::size_t Chunks() const { return pool.size() + full.size() + (current != nullptr ? 1 : 0); }

private:
    std::unique_ptr<std::byte[]> take() {
        if (pool.empty()) return std::make_unique<std::byte[]>(ChunkBytes);
        auto chunk = std::move(pool.back());
        pool.pop_back();
        return chunk;
    }

    static void play(std::byte* chunk, std::size_t end) {
        for (std::size_t at = 0; at < end;) {
            const auto* header = reinterpret_cast<const Header*>(chunk + at);
            header->replay(chunk + at + sizeof(Header));
            at += sizeof(Header) + header->bytes;
        }
    }

    std::unique_ptr<std::byte[]> current;
    std::size_t used = 0;
    std::vector<std::pair<std::unique_ptr<std::byte[]>, std::size_t>> full;
    std::vector<std::unique_ptr<std::byte[]>> pool;
};

Stream stream;
Stats stats;
std::atomic<bool> pending{false};
std::atomic<std::uint64_t> unguardedCommands{0}, unguardedOther{0}, presents{0};
std::uint32_t emitTick = 0;
PFN_vkGetDeviceProcAddr realDeviceProc = nullptr;

void report() {
    const auto tenSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
    if (stats.reportedAt == tenSeconds) return;
    const bool first = stats.reportedAt == 0;
    stats.reportedAt = tenSeconds;
    if (first) return;
    const double draws = stats.draws != 0 ? static_cast<double>(stats.draws) : 1.0;
    std::string syncs;
    for (const auto& [name, count] : stats.syncs) syncs += " " + name + " " + std::to_string(count) + ";";
    std::fprintf(stderr, "[r-inline] 10 s: %llu entries (%.1f per draw/dispatch), %llu bytes (%.0f per draw/dispatch), %llu draws/dispatches; emit %.1f ms (1-of-64 x64), replay %.1f ms, %llu flushes, %llu presents, %llu chunks; calls from threads without the GPU lock while entries were pending: %llu command buffer calls (the run is invalid if not 0), %llu others; syncs that flushed:%s\n", static_cast<unsigned long long>(stats.entries), static_cast<double>(stats.entries) / draws, static_cast<unsigned long long>(stats.bytes), static_cast<double>(stats.bytes) / draws, static_cast<unsigned long long>(stats.draws), static_cast<double>(stats.emitNs) * 64.0 / 1e6, static_cast<double>(stats.replayNs) / 1e6, static_cast<unsigned long long>(stats.flushes), static_cast<unsigned long long>(presents.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(stream.Chunks()), static_cast<unsigned long long>(unguardedCommands.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(unguardedOther.exchange(0, std::memory_order_relaxed)), syncs.c_str());
    const auto reportedAt = stats.reportedAt;
    stats = {};
    stats.reportedAt = reportedAt;
}

bool emitting() {
    return Recorder::OnCommitter() && GuestMemory::GpuMutex().HeldByThisThread();
}

void flush(const char* reason) {
    if (!pending.load(std::memory_order_acquire)) return;
    const auto start = std::chrono::steady_clock::now();
    stream.Play();
    pending.store(false, std::memory_order_release);
    stats.replayNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    ++stats.flushes;
    ++stats.syncs[reason];
    report();
}

void syncFor(const char* name) {
    if (!pending.load(std::memory_order_acquire)) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) {
        flush(name);
        return;
    }
    const bool command = std::strncmp(name, "vkCmd", 5) == 0 || std::strstr(name, "CommandBuffer") != nullptr || std::strstr(name, "CommandPool") != nullptr || std::strncmp(name, "vkQueueSubmit", 13) == 0;
    (command ? unguardedCommands : unguardedOther).fetch_add(1, std::memory_order_relaxed);
}

void flushBeforeRelease() {
    flush("GPU lock release");
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

template<typename T>
std::size_t descriptorBytes(const VkWriteDescriptorSet& write) {
    return padded(write.descriptorCount * sizeof(T));
}

template<typename Args>
struct Writer {
    std::byte* at;

    template<std::size_t P, typename Count>
    void array(Args& args, Count count) {
        auto& pointer = std::get<P>(args);
        using T = std::remove_const_t<std::remove_pointer_t<std::remove_reference_t<decltype(pointer)>>>;
        if (pointer == nullptr || count == 0) return;
        const auto bytes = static_cast<std::size_t>(count) * sizeof(T);
        std::memcpy(at, pointer, bytes);
        pointer = reinterpret_cast<const T*>(at);
        at += padded(bytes);
    }

    template<std::size_t P>
    void bytes(Args& args, std::size_t count) {
        auto& pointer = std::get<P>(args);
        if (pointer == nullptr || count == 0) return;
        std::memcpy(at, pointer, count);
        pointer = at;
        at += padded(count);
    }
};

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
        return items == nullptr ? 0 : padded(count * sizeof(*items));
    }
    template<typename Args>
    static void Copy(Args& args, Writer<Args>& writer) {
        writer.template array<P>(args, std::get<N>(args));
    }
};

template<std::size_t P>
struct OneAt {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        return std::get<P>(args) == nullptr ? 0 : padded(sizeof(*std::get<P>(args)));
    }
    template<typename Args>
    static void Copy(Args& args, Writer<Args>& writer) {
        writer.template array<P>(args, 1u);
    }
};

template<std::size_t P, std::size_t N>
struct BytesAt {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        return std::get<P>(args) == nullptr ? 0 : padded(static_cast<std::size_t>(std::get<N>(args)));
    }
    template<typename Args>
    static void Copy(Args& args, Writer<Args>& writer) {
        writer.template bytes<P>(args, static_cast<std::size_t>(std::get<N>(args)));
    }
};

struct ByValue {
    template<typename Args>
    static std::size_t Size(const Args&) { return 0; }
    template<typename Args>
    static void Copy(Args&, Writer<Args>&) {}
};

struct Sync {};

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
    static void Copy(Args& args, Writer<Args>& writer) {
        (Parts::Copy(args, writer), ...);
    }
};

struct DescriptorWrites {
    template<typename Args>
    static std::size_t Size(const Args& args) {
        const auto count = std::get<1>(args);
        const auto* writes = std::get<2>(args);
        std::size_t total = padded(count * sizeof(VkWriteDescriptorSet)) + padded(std::get<3>(args) * sizeof(VkCopyDescriptorSet));
        if (chained(writes, count) || chained(std::get<4>(args), std::get<3>(args))) return Cannot;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& write = writes[index];
            const auto kind = descriptorKind(write.descriptorType);
            if (kind < 0) return Cannot;
            total += kind == 0 ? descriptorBytes<VkDescriptorImageInfo>(write) : kind == 1 ? descriptorBytes<VkDescriptorBufferInfo>(write) : descriptorBytes<VkBufferView>(write);
        }
        return total;
    }
    template<typename Args>
    static void Copy(Args& args, Writer<Args>& writer) {
        writer.template array<2>(args, std::get<1>(args));
        writer.template array<4>(args, std::get<3>(args));
        auto* writes = const_cast<VkWriteDescriptorSet*>(std::get<2>(args));
        for (std::uint32_t index = 0; writes != nullptr && index < std::get<1>(args); ++index) {
            auto& write = writes[index];
            const auto kind = descriptorKind(write.descriptorType);
            const auto copy = [&](auto*& pointer, bool used) {
                using T = std::remove_const_t<std::remove_pointer_t<std::remove_reference_t<decltype(pointer)>>>;
                if (!used) pointer = nullptr;
                if (pointer == nullptr) return;
                std::memcpy(writer.at, pointer, write.descriptorCount * sizeof(T));
                pointer = reinterpret_cast<const T*>(writer.at);
                writer.at += padded(write.descriptorCount * sizeof(T));
            };
            copy(write.pImageInfo, kind == 0);
            copy(write.pBufferInfo, kind == 1);
            copy(write.pTexelBufferView, kind == 2);
        }
    }
};

template<std::size_t Slot, typename F, typename Copier>
struct Deferred;

template<std::size_t Slot, typename Copier, typename... A>
struct Deferred<Slot, void(VKAPI_PTR*)(A...), Copier> {
    using Args = std::tuple<A...>;
    static inline void(VKAPI_PTR* real)(A...) = nullptr;
    static inline const char* name = nullptr;
    static inline std::string refused;
    static inline bool draw = false;

    static void Play(const std::byte* payload) {
        std::apply(real, *reinterpret_cast<const Args*>(payload));
    }

    static void VKAPI_CALL Call(A... arguments) {
        if constexpr (std::is_same_v<Copier, Sync>) {
            syncFor(name);
            return real(arguments...);
        } else {
            if (!emitting()) {
                syncFor(name);
                return real(arguments...);
            }
            const bool sampled = (emitTick++ & 63u) == 0;
            const auto start = sampled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            Args args{arguments...};
            const auto more = Copier::Size(args);
            std::byte* at = more == Cannot ? nullptr : stream.Reserve(&Play, padded(sizeof(Args)) + more);
            if (at == nullptr) {
                flush(refused.c_str());
                return real(arguments...);
            }
            pending.store(true, std::memory_order_release);
            auto* stored = new (at) Args(args);
            Writer<Args> writer{at + padded(sizeof(Args))};
            Copier::Copy(*stored, writer);
            ++stats.entries;
            stats.bytes += sizeof(Header) + padded(sizeof(Args)) + more;
            if (draw) ++stats.draws;
            if (sampled) {
                stats.emitNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
                ++stats.emitSampled;
            }
        }
    }
};

template<std::size_t Slot, typename Copier, typename R, typename... A>
struct Deferred<Slot, R(VKAPI_PTR*)(A...), Copier> {
    static inline R(VKAPI_PTR* real)(A...) = nullptr;
    static inline const char* name = nullptr;
    static inline bool draw = false;

    static R VKAPI_CALL Call(A... arguments) {
        syncFor(name);
        return real(arguments...);
    }
};

template<std::size_t Slot, typename F, typename Copier>
PFN_vkVoidFunction wrap(PFN_vkVoidFunction real, const char* name) {
    using Wrapper = Deferred<Slot, F, Copier>;
    Wrapper::real = reinterpret_cast<F>(real);
    Wrapper::name = name;
    if constexpr (requires { Wrapper::refused; }) {
        if (Wrapper::refused.empty()) Wrapper::refused = std::string("refused:") + name;
    }
    Wrapper::draw = std::strncmp(name, "vkCmdDraw", 9) == 0 || std::strncmp(name, "vkCmdDispatch", 13) == 0;
    return reinterpret_cast<PFN_vkVoidFunction>(&Wrapper::Call);
}

PFN_vkVoidFunction present(PFN_vkVoidFunction real) {
    static PFN_vkQueuePresentKHR target = nullptr;
    target = reinterpret_cast<PFN_vkQueuePresentKHR>(real);
    return reinterpret_cast<PFN_vkVoidFunction>(+[](VkQueue queue, const VkPresentInfoKHR* info) -> VkResult {
        syncFor("vkQueuePresentKHR");
        presents.fetch_add(1, std::memory_order_relaxed);
        return target(queue, info);
    });
}

PFN_vkVoidFunction VKAPI_CALL inlineDeviceProc(VkDevice device, const char* name) {
    const auto real = realDeviceProc(device, name);
    if (real == nullptr) return real;
#define X(n, ...) \
    if (std::strcmp(name, #n) == 0) return wrap<__COUNTER__, PFN_##n, __VA_ARGS__>(real, #n);
    X(vkCmdBeginQuery, ByValue)
    X(vkCmdBeginRenderPass, Sync)
    X(vkCmdBeginRenderingKHR, Sync)
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
    X(vkBeginCommandBuffer, Sync)
    X(vkEndCommandBuffer, Sync)
    X(vkResetCommandBuffer, Sync)
    X(vkResetCommandPool, Sync)
    X(vkFreeCommandBuffers, Sync)
    X(vkQueueSubmit, Sync)
    X(vkQueueSubmit2KHR, Sync)
    X(vkQueueWaitIdle, Sync)
    X(vkDeviceWaitIdle, Sync)
    X(vkWaitForFences, Sync)
    X(vkGetFenceStatus, Sync)
    X(vkResetFences, Sync)
    X(vkGetQueryPoolResults, Sync)
    X(vkWaitSemaphoresKHR, Sync)
    X(vkGetSemaphoreCounterValueKHR, Sync)
    X(vkResetDescriptorPool, Sync)
    X(vkFreeDescriptorSets, Sync)
    X(vkDestroyDescriptorPool, Sync)
    X(vkDestroyBuffer, Sync)
    X(vkDestroyBufferView, Sync)
    X(vkDestroyImage, Sync)
    X(vkDestroyImageView, Sync)
    X(vkDestroySampler, Sync)
    X(vkDestroyPipeline, Sync)
    X(vkDestroyPipelineLayout, Sync)
    X(vkDestroyRenderPass, Sync)
    X(vkDestroyFramebuffer, Sync)
    X(vkDestroyQueryPool, Sync)
    X(vkFreeMemory, Sync)
    X(vkUnmapMemory, Sync)
    X(vkDestroyCommandPool, Sync)
    X(vkDestroyDevice, Sync)
#undef X
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) return present(real);
    return real;
}

}

PFN_vkGetDeviceProcAddr InlineDeviceProc(PFN_vkGetDeviceProcAddr real) {
    if (std::getenv("APS5_R_INLINE") == nullptr) return real;
    realDeviceProc = real;
    GuestMemory::SetGpuReleaseHook(&flushBeforeRelease);
    std::fprintf(stderr, "[r-inline] on: the committer's Vulkan calls under the GPU lock go into a stream that it replays at the next sync point\n");
    return &inlineDeviceProc;
}

}
