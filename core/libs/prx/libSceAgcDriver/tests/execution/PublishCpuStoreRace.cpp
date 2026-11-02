#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint64_t Unit = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Published = 0x5ad0beefu;
constexpr std::uint32_t Marker = 0xc0ffee01u;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "publish CPU store race: cannot allocate the guest block");
        std::fill(Words(), Words() + BlockBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), BlockBytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

class SubmissionGate {
public:
    explicit SubmissionGate(AgcDriver::VulkanDevice& device) : device(device) {
#ifdef _WIN32
        constexpr const char* libraryName = "vulkan-1.dll";
#else
        constexpr const char* libraryName = "libvulkan.so.1";
#endif
        library.reset(SDL_LoadObject(libraryName));
        Require(library != nullptr, "publish CPU store race: cannot load Vulkan for the submission gate");
        resolve = reinterpret_cast<PFN_vkGetDeviceProcAddr>(SDL_LoadFunction(library.get(), "vkGetDeviceProcAddr"));
        Require(resolve != nullptr, "publish CPU store race: cannot resolve vkGetDeviceProcAddr");
        const auto createSemaphore = function<PFN_vkCreateSemaphore>("vkCreateSemaphore");
        destroySemaphore = function<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        signalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(resolve(device.Device(), "vkSignalSemaphoreKHR"));
        if (signalSemaphore == nullptr) signalSemaphore = function<PFN_vkSignalSemaphore>("vkSignalSemaphore");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        Require(createSemaphore(device.Device(), &info, nullptr, &semaphore) == VK_SUCCESS, "publish CPU store race: cannot create the submission gate");
    }

    ~SubmissionGate() {
        Release();
        try {
            device.WaitIdle();
        } catch (...) {
        }
        destroySemaphore(device.Device(), semaphore, nullptr);
    }

    SubmissionGate(const SubmissionGate&) = delete;
    SubmissionGate& operator=(const SubmissionGate&) = delete;

    void Arm(AgcDriver::Graphics::Recorder& recorder) {
        recorder.Commands();
        recorder.HoldNextSubmit(semaphore, 1);
    }

    void Release() {
        if (signaled.exchange(true)) return;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore;
        info.value = 1;
        Require(signalSemaphore(device.Device(), &info) == VK_SUCCESS, "publish CPU store race: cannot release the submission gate");
    }

private:
    template <typename TFunction>
    TFunction function(const char* name) const {
        const auto function = reinterpret_cast<TFunction>(resolve(device.Device(), name));
        Require(function != nullptr, std::string("publish CPU store race: cannot resolve ") + name);
        return function;
    }

    AgcDriver::VulkanDevice& device;
    std::unique_ptr<void, decltype(&SDL_UnloadObject)> library{nullptr, &SDL_UnloadObject};
    PFN_vkGetDeviceProcAddr resolve = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkSignalSemaphore signalSemaphore = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::atomic<bool> signaled{false};
};

void StoreMarkers(std::uint64_t unit) {
    for (std::uint64_t at = unit; at < unit + Unit; at += Page) *reinterpret_cast<volatile std::uint32_t*>(at) = Marker;
}

std::atomic<std::uint64_t> selectedUnit{0};
std::atomic<bool> storedAtSelection{false};

void StoreAtSelection(std::uint64_t begin, std::uint64_t) {
    const auto unit = selectedUnit.load();
    if (unit == 0 || begin > unit || unit >= begin + Unit) return;
    std::thread writer(StoreMarkers, unit);
    writer.join();
    storedAtSelection = true;
}

enum class StoreAt { AfterRecord, AtSelection };

struct Outcome {
    std::size_t lostPages = 0;
    std::size_t keptPages = 0;
    bool storesFinishedBeforeRelease = false;
    std::size_t consumerPagesUnpublished = 0;
};

Outcome Race(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::uint64_t offset, StoreAt storeAt) {
    using namespace AgcDriver::Graphics;
    const auto unit = guest.Address() + offset;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    auto consumer = std::make_shared<Buffer>(context, static_cast<std::size_t>(Unit), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Outcome outcome;
    SubmissionGate gate(device);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto* import = HostImportFor(context, unit, static_cast<std::size_t>(Unit));
        Require(import != nullptr, "publish CPU store race: the unit has no host import");
        auto* recorder = Recorder::Active();
        Require(recorder != nullptr, "publish CPU store race: no active recorder");
        Require(AgcDriver::GuestMemory::Watched(unit, Unit), "publish CPU store race: the unit is outside the write watch");
        AgcDriver::GuestMemory::CollectWritesUncached(unit, Unit);
        const auto destination = ShadowDestinationFor(context, *import, unit, unit + Unit);
        Require(destination.has_value(), "publish CPU store race: no unit shadow for the unit");
        gate.Arm(*recorder);
        const auto commands = recorder->Commands();
        recorder->Keep(destination->slab);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, destination->buffer, destination->offset, Unit, Published);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        recorder->MarkCovered(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const ShadowedRange range{unit, unit + Unit, destination->slab};
        MarkShadowed(*import, std::span(&range, 1), AgcDriver::GuestMemory::TrackerGeneration());
        if (storeAt == StoreAt::AtSelection) {
            selectedUnit = unit;
            storedAtSelection = false;
            SetPublishSelectedHook(StoreAtSelection);
        }
        const auto published = PublishShadow(unit, Unit, PublishScope::Whole, PublishReason::Hook);
        SetPublishSelectedHook(nullptr);
        selectedUnit = 0;
        if (storeAt == StoreAt::AfterRecord) Require(published != 0, "publish CPU store race: the shadowed unit was not published");
        else Require(storedAtSelection, "publish CPU store race: the publish did not reach its selection hook");
        const auto reads = recorder->Commands();
        RecordMemoryBarrier(context, reads, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, reads, import->buffer, HostImportOffset(*import, unit), consumer->Handle(), 0, Unit);
        RecordMemoryBarrier(context, reads, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder->Keep(consumer);
        recorder->Submit();
    }
    std::atomic<bool> stored{storeAt == StoreAt::AtSelection};
    std::thread writer;
    if (storeAt == StoreAt::AfterRecord) {
        writer = std::thread([&] {
            StoreMarkers(unit);
            stored = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!stored && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    outcome.storesFinishedBeforeRelease = stored;
    gate.Release();
    device.WaitIdle();
    if (writer.joinable()) writer.join();
    consumer->Invalidate();
    const auto* seen = reinterpret_cast<const std::uint32_t*>(consumer->Bytes().data());
    for (std::uint64_t at = unit; at < unit + Unit; at += Page) {
        const auto value = *reinterpret_cast<volatile const std::uint32_t*>(at);
        (value == Marker ? outcome.keptPages : outcome.lostPages) += 1;
        if (storeAt == StoreAt::AfterRecord && seen[(at - unit) / 4u + 1u] != Published) ++outcome.consumerPagesUnpublished;
    }
    return outcome;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::Graphics::UnitShadowEnabled()) {
            std::puts("guest memory has no write watch or unit shadows are off: publish CPU store race tests skipped");
            return VulkanTestSkipped;
        }
        GuestBlock guest;
        std::size_t changed = 0;
        const struct {
            StoreAt storeAt;
            std::uint64_t offset;
            const char* name;
        } cases[] = {{StoreAt::AfterRecord, Unit, "CPU stores after the publish copy was recorded"}, {StoreAt::AtSelection, 4 * Unit, "CPU stores between the publish selection and the copy"}};
        for (const auto& race : cases) {
            const auto outcome = Race(*device, guest, race.offset, race.storeAt);
            std::printf("%s: %zu of %zu stored pages kept the CPU marker, %zu lost it to the publish copy; stores finished before the GPU ran: %s; GPU reader pages without the published bytes: %zu\n", race.name, outcome.keptPages, outcome.keptPages + outcome.lostPages, outcome.lostPages, outcome.storesFinishedBeforeRelease ? "yes" : "no", outcome.consumerPagesUnpublished);
            const auto pages = static_cast<std::size_t>(Unit / Page);
            if (!outcome.storesFinishedBeforeRelease || outcome.lostPages != pages || outcome.keptPages != 0 || outcome.consumerPagesUnpublished != 0) ++changed;
        }
        if (changed != 0) {
            std::cerr << "publish CPU store race: the known gap (a shadow publish copy recorded before CPU stores overwrites them) changed in " << changed << " case(s); update this test and its debt entry\n";
            return 1;
        }
        std::puts("publish CPU store race: the known gap reproduced in both cases (CPU stores lost to the shadow publish copy)");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
