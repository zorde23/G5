#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#include "FixtureDeviceSelection.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr VkExtent2D Extent{64, 64};

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#elif defined(__APPLE__)
        library = SDL_LoadObject("libvulkan.1.dylib");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
#ifdef __APPLE__
            const char* portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &portability;
#endif
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = SelectFixturePhysicalDevice(devices, function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties"));
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) ++family;
            Require(family < count, "no Vulkan graphics queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.device != VK_NULL_HANDLE) ClearDepthSurfaces(context.device);
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

constexpr std::size_t Block = 65536;

void* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), bytes), "the test block is not watched");
    return block;
}

struct Calls {
    const Context& context;

    // One DepthSurfaceView call that must (hit) or must not (miss) be answered from the memo.
    VkImageView Expect(const DepthTarget& target, bool depthWritten, bool hit, const char* what) const {
        const auto before = DepthViewMemoHits();
        const auto view = DepthSurfaceView(context, target, depthWritten);
        const auto hits = DepthViewMemoHits() - before;
        Require(hits == (hit ? 1u : 0u), std::string(what) + (hit ? ": the call did not take the memo" : ": the call took the memo"));
        return view;
    }
};

void Run(const Context& context, std::uint64_t watched, bool mirrorable) {
    namespace GuestMemory = AgcDriver::GuestMemory;
    auto* memory = reinterpret_cast<volatile std::uint8_t*>(watched);
    DepthTarget target{watched, 0, Extent, VK_FORMAT_D32_SFLOAT, 1.0f, 0, watched + Block};
    if (mirrorable) target.depthSwizzle = XorSwizzleMode(TextureTileMode::kZ64KBX);
    const Calls calls{context};
    GuestMemory::BumpCollectEpoch();

    const auto view = calls.Expect(target, true, false, "the first call");
    Require(view != VK_NULL_HANDLE, "the depth surface has no view");
    Require(calls.Expect(target, true, true, "the same target again") == view, "the memo returned another view");
    Require(calls.Expect(target, true, true, "the same target a third time") == view, "the memo returned another view the second time");

    memory[8] = 0x5a;
    GuestMemory::BumpCollectEpoch();
    Require(calls.Expect(target, true, false, "a CPU write to the depth plane and a new collect epoch") == view, "the depth surface changed after a CPU write");
    calls.Expect(target, true, true, "the same target after the CPU write was synced");

    GuestMemory::BreakProofs();
    calls.Expect(target, true, false, "a broken proof");
    calls.Expect(target, true, true, "the same target after the proof was taken again");

    if (mirrorable) {
        // A collect of a range that covers the plane stamps the CPU write within the same epoch.
        memory[16] = 0xa5;
        GuestMemory::CollectWrites(watched, 2 * Block);
        calls.Expect(target, true, false, "a CPU write stamped on the plane by an overlapping collect");
        calls.Expect(target, true, true, "the same target after the stamped write was synced");
    }

    auto cleared = target;
    cleared.clearDepth = 0.5f;
    Require(calls.Expect(cleared, true, false, "another clear depth") == view, "another clear depth made another surface");
    calls.Expect(cleared, true, true, "the same clear depth again");
    calls.Expect(cleared, false, false, "a call that does not write depth");
    calls.Expect(cleared, false, true, "a second call that does not write depth");
    calls.Expect(cleared, true, false, "a call that writes depth again");

    Require(NoteHtileDepthClear(target.htileAddress, static_cast<std::size_t>(HtileMetadataBytes(Extent))) == false, "a single-sample HTILE clear queued a completion");
    calls.Expect(cleared, true, false, "an HTILE fast clear between two draws");
    calls.Expect(cleared, true, true, "the same target after the fast clear was applied");

    RetireDepthSurfaces(context.device, watched, 0x1000);
    Require(calls.Expect(cleared, true, false, "a retired depth surface") == view, "the retired depth surface was not brought back");
    calls.Expect(cleared, true, true, "the same target after the surface was brought back");

    RuntimeSwitches::Assign(RuntimeSwitch::DepthViewMemo, false);
    calls.Expect(cleared, true, false, "the switch unset");
    calls.Expect(cleared, true, false, "the switch unset, a second call");
    RuntimeSwitches::Assign(RuntimeSwitch::DepthViewMemo, true);
    calls.Expect(cleared, true, false, "the switch set again");
    calls.Expect(cleared, true, true, "the switch set again, a second call");

    // sameSurface ignores a single-sample target's swizzle: the surface is bound with the other one.
    auto swapped = cleared;
    swapped.depthSwizzle = mirrorable ? 0u : XorSwizzleMode(TextureTileMode::kZ64KBX);
    Require(calls.Expect(swapped, true, false, "the other swizzle") == view, "the other swizzle made another surface");
    calls.Expect(swapped, true, true, "the other swizzle again");
}

// With APS5_VERIFY_DRAW_PIPELINE every 64th hit also runs the full path and aborts where its
// surface or stale units differ from the memo's; the hit counts above would be one short there, so
// this run only loops. A four-unit plane made with one swizzle and bound with the other shows a
// repeat that marks the units of the surface's own target instead of the call's; the first loop is
// 191 hits long so that the first hit of the swapped binding is the 192nd, a verified one, while
// the surface still holds the units the full path left.
void Verify(const Context& context, std::uint64_t watched) {
    constexpr VkExtent2D Wide{256, 256};
    Require(DepthSliceBytes(Wide, 4) == 4 * Block, "the verified plane is not four units");
    const DepthTarget plain{watched, 0, Wide, VK_FORMAT_D32_SFLOAT, 1.0f, 0, watched + 4 * Block};
    auto tiled = plain;
    tiled.depthSwizzle = XorSwizzleMode(TextureTileMode::kZ64KBX);
    std::uint64_t eligible = 0;
    const auto before = DepthViewMemoHits();
    for (const auto& [made, bound, calls] : {std::tuple{plain, plain, 190}, std::tuple{tiled, plain, 192}, std::tuple{plain, tiled, 192}}) {
        AgcDriver::GuestMemory::BumpCollectEpoch();
        const auto view = DepthSurfaceView(context, made);
        Require(DepthSurfaceView(context, bound) == view, "the bound swizzle made another surface");
        for (int call = 0; call < calls; ++call) Require(DepthSurfaceView(context, bound) == view, "a verified memo call returned another view");
        eligible += static_cast<std::uint64_t>(calls) + (made.depthSwizzle == bound.depthSwizzle ? 1u : 0u);
        ClearDepthSurfaces(context.device);
    }
    Require(DepthViewMemoHits() - before == eligible - eligible / 64u, "the verified calls did not take the memo all but every 64th time");
}

}

int main() {
    try {
        std::unique_ptr<Device> device;
        try {
            device = std::make_unique<Device>();
        } catch (const std::exception& error) {
            if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
            std::printf("skipped, no usable Vulkan device: %s\n", error.what());
            return VulkanTestSkipped;
        }
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::puts("skipped, guest memory is not write-watched here");
            return VulkanTestSkipped;
        }
        auto context = device->GetContext();
        TextureDetiler detiler(context);
        context.detiler = &detiler;
        const auto watched = reinterpret_cast<std::uint64_t>(AllocateWatched(5 * Block));
        AgcDriver::GuestMemory::MarkProofThread();
        RuntimeSwitches::Assign(RuntimeSwitch::DepthViewMemo, true);
        if (std::getenv("APS5_VERIFY_DRAW_PIPELINE") != nullptr) {
            Verify(context, watched);
            ClearDepthSurfaces(context.device);
            std::puts("depth view memo verify passed");
            return 0;
        }
        for (const bool mirrorable : {false, true}) {
            Run(context, watched, mirrorable);
            ClearDepthSurfaces(context.device);
        }
        std::puts("depth view memo tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
