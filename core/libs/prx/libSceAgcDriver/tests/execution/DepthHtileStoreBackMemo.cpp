#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>

namespace {

using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::HtileBytes;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Surface;
using DepthFastClearHarness::Width;

constexpr std::size_t Block = 65536;
constexpr std::uint32_t Expanded = 0xfffc000fu;

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

void QueueHtileWriteBack(const AgcDriver::Graphics::Context& context, AgcDriver::Graphics::Recorder& recorder, std::uint64_t address) {
    auto buffer = std::make_shared<AgcDriver::Graphics::Buffer>(context, HtileBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer->Handle(), 0u, HtileBytes, Expanded);
    AgcDriver::Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.OnComplete([buffer, address] {
        buffer->Invalidate();
        AgcDriver::GuestMemory::StoreOwnBytes(address, buffer->Bytes().first(HtileBytes));
    });
    recorder.Keep(buffer);
    recorder.NotePendingWrite(address, HtileBytes);
}

}

int main() {
    try {
        Require(AgcDriver::Graphics::DepthSliceBytes({Width, Height}, sizeof(float)) <= Block && HtileBytes <= Block, "the fixtures do not fit their watched blocks");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::puts("skipped, guest memory is not write-watched here");
            return VulkanTestSkipped;
        }
        const auto shaders = Compile(*device);
        const auto depthAddress = reinterpret_cast<std::uint64_t>(AllocateWatched(2 * Block));
        const auto htileAddress = depthAddress + Block;
        const auto htile = std::span(reinterpret_cast<std::uint32_t*>(htileAddress), HtileBytes / 4u);
        std::fill(htile.begin(), htile.end(), Expanded);
        const Surface depth{depthAddress, 0, htileAddress, false, VK_FORMAT_D32_SFLOAT};

        Require(Draw(*device, shaders, depth, {.depthWrite = true}) == Covered, "a new depth surface was not cleared to DB_DEPTH_CLEAR (1.0)");
        const auto first = AgcDriver::Graphics::HtileStoreBackReads();
        Require(Draw(*device, shaders, depth, {}) == 0u, "a draw at depth 0.5 passed LESS against the 0.5 written before it");
        Require(AgcDriver::Graphics::HtileStoreBackReads() == first, "an HTILE nobody wrote since its last read was read again");

        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder != nullptr, "the test device has no active recorder");
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            QueueHtileWriteBack(device->graphicsContext(), *recorder, htileAddress);
        }
        Require(Draw(*device, shaders, depth, {}) == 0u, "the depth was cleared by an HTILE written back expanded");
        const auto afterWrite = AgcDriver::Graphics::HtileStoreBackReads();
        Require(afterWrite == first + 1u, "an HTILE with a recorded GPU write over it was not read once");
        Require(Draw(*device, shaders, depth, {}) == 0u, "the depth was cleared on the draw after the write-back");
        Require(AgcDriver::Graphics::HtileStoreBackReads() == afterWrite, "the HTILE read after its GPU write landed was read again: the memo kept a generation from before the write-back");
        std::puts("depth HTILE store-back memo tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
