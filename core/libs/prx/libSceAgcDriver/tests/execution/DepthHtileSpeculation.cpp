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
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>

namespace {

using AgcDriver::Graphics::HtileSpeculationCounters;
using AgcDriver::Graphics::HtileStoreBackReads;
using AgcDriver::Graphics::Recorder;
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
constexpr std::uint32_t Cleared = 0xfffc0000u;
constexpr std::uint32_t StencilExpanded = 0xfffff3ffu;
constexpr std::uint64_t Program = 0x10000u;

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

void QueueHtileWriteBack(const AgcDriver::Graphics::Context& context, Recorder& recorder, std::uint64_t address, std::uint32_t word, std::uint64_t dataHash, std::size_t bytes = HtileBytes) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto buffer = std::make_shared<AgcDriver::Graphics::Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer->Handle(), 0u, bytes, word);
    AgcDriver::Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.OnComplete([buffer, address, bytes] {
        buffer->Invalidate();
        AgcDriver::GuestMemory::StoreOwnBytes(address, buffer->Bytes().first(bytes));
    });
    recorder.Keep(buffer);
    AgcDriver::GuestMemory::SetCurrentProgram(Program);
    recorder.SetWriteDataHash(dataHash);
    recorder.NotePendingWrite(address, bytes);
    recorder.SetWriteDataHash(0);
    AgcDriver::GuestMemory::SetCurrentProgram(0);
}

bool Holds(std::span<const std::uint32_t> htile, std::uint32_t word) {
    return std::all_of(htile.begin(), htile.end(), [&](std::uint32_t value) { return value == word; });
}

}

int main() {
    try {
#ifdef _WIN32
        Require(_putenv_s("APS5_HTILE_SPECULATE", "1") == 0, "cannot set APS5_HTILE_SPECULATE");
#else
        Require(setenv("APS5_HTILE_SPECULATE", "1", 1) == 0, "cannot set APS5_HTILE_SPECULATE");
#endif
        Require(AgcDriver::Graphics::DepthSliceBytes({Width, Height}, sizeof(float)) <= Block && HtileBytes <= Block, "the fixtures do not fit their watched blocks");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::puts("skipped, guest memory is not write-watched here");
            return VulkanTestSkipped;
        }
        const auto shaders = Compile(*device);
        const auto context = device->graphicsContext();
        const auto depthAddress = reinterpret_cast<std::uint64_t>(AllocateWatched(2 * Block));
        const auto htileAddress = depthAddress + Block;
        const auto htile = std::span(reinterpret_cast<std::uint32_t*>(htileAddress), HtileBytes / 4u);
        std::fill(htile.begin(), htile.end(), Expanded);
        const Surface depth{depthAddress, 0, htileAddress, false, VK_FORMAT_D32_SFLOAT};
        Require(Draw(*device, shaders, depth, {.depthWrite = true}) == Covered, "a new depth surface was not cleared to DB_DEPTH_CLEAR (1.0)");
        auto* recorder = Recorder::Active();
        Require(recorder != nullptr, "the test device has no active recorder");

        auto reads = HtileStoreBackReads();
        auto counts = HtileSpeculationCounters();
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 1u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "an expanded HTILE written by the GPU cleared the depth");
        Require(HtileStoreBackReads() == reads + 1u && HtileSpeculationCounters().speculated == counts.speculated, "the first write of a writer was not read exactly");
        reads = HtileStoreBackReads();
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 1u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "a speculated HTILE cleared the depth");
        Require(HtileStoreBackReads() == reads, "a writer that left the HTILE not cleared was read again instead of speculated");
        const auto speculated = HtileSpeculationCounters();
        Require(speculated.speculated == counts.speculated + 1u && speculated.verified == counts.verified + 1u && speculated.diverged == counts.diverged, "the speculation was not verified once without divergence");

        QueueHtileWriteBack(context, *recorder, htileAddress, Cleared, 1u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "a speculated draw applied the clear its writer stored");
        const auto diverged = HtileSpeculationCounters();
        Require(diverged.speculated == speculated.speculated + 1u && diverged.diverged == speculated.diverged + 1u && diverged.banned == speculated.banned + 1u && diverged.rewritten == speculated.rewritten + 1u, "a writer that stored a fast clear did not diverge, ban and rewrite once");
        Require(Holds(htile, Expanded), "the missed fast clear was not consumed by rewriting the HTILE expanded");
        reads = HtileStoreBackReads();
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 1u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "the draw after the ban cleared the depth");
        Require(HtileStoreBackReads() == reads + 1u && HtileSpeculationCounters().speculated == diverged.speculated, "a banned writer was speculated again");

        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 2u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "an expanded HTILE of a second writer cleared the depth");
        htile[0] = Expanded;
        reads = HtileStoreBackReads();
        const auto beforeCpu = HtileSpeculationCounters();
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 2u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "the draw after a CPU store into the HTILE cleared the depth");
        Require(HtileStoreBackReads() == reads + 1u && HtileSpeculationCounters().speculated == beforeCpu.speculated, "a CPU store into the HTILE since the last observation did not stop the speculation");

        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 3u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "an expanded HTILE of a third writer cleared the depth");
        const auto beforeNewer = HtileSpeculationCounters();
        QueueHtileWriteBack(context, *recorder, htileAddress, Cleared, 3u);
        const AgcDriver::Graphics::DepthTarget target{depth.depth, depth.stencil, {Width, Height}, depth.format, 1.0f, 0, depth.htile, depth.htileStencil};
        static_cast<void>(AgcDriver::Graphics::DepthSurfaceView(context, target, false));
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            recorder->Submit();
        }
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 4u);
        device->WaitIdle();
        const auto newer = HtileSpeculationCounters();
        Require(newer.speculated == beforeNewer.speculated + 1u && newer.diverged == beforeNewer.diverged + 1u && newer.banned == beforeNewer.banned + 1u && newer.rewritten == beforeNewer.rewritten, "a divergence under a newer recorded write was rewritten or not banned");
        Require(Holds(htile, Expanded), "the newer write did not land after the divergence");

        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 5u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "an expanded HTILE of a fifth writer cleared the depth");
        const auto beforeSameBatch = HtileSpeculationCounters();
        QueueHtileWriteBack(context, *recorder, htileAddress, Cleared, 5u);
        static_cast<void>(AgcDriver::Graphics::DepthSurfaceView(context, target, false));
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 6u);
        device->WaitIdle();
        const auto sameBatch = HtileSpeculationCounters();
        Require(sameBatch.speculated == beforeSameBatch.speculated + 1u && sameBatch.diverged == beforeSameBatch.diverged + 1u && sameBatch.rewritten == beforeSameBatch.rewritten, "a divergence under a newer write of the same batch was rewritten");

        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 7u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "an expanded HTILE of a seventh writer cleared the depth");
        reads = HtileStoreBackReads();
        const auto beforePartial = HtileSpeculationCounters();
        QueueHtileWriteBack(context, *recorder, htileAddress, Expanded, 7u, HtileBytes / 2u);
        Require(Draw(*device, shaders, depth, {}) == 0u, "the draw after a partial HTILE write cleared the depth");
        Require(HtileStoreBackReads() == reads + 1u && HtileSpeculationCounters().speculated == beforePartial.speculated, "a writer covering only part of the HTILE was speculated");

        const auto stencilAddress = reinterpret_cast<std::uint64_t>(AllocateWatched(Block));
        std::fill(htile.begin(), htile.end(), StencilExpanded);
        const Surface stencil{depthAddress, stencilAddress, htileAddress, true, VK_FORMAT_D32_SFLOAT_S8_UINT};
        static_cast<void>(Draw(*device, shaders, stencil, {.depthWrite = true}));
        const auto beforeStencil = HtileSpeculationCounters();
        for (int round = 0; round < 2; ++round) {
            reads = HtileStoreBackReads();
            QueueHtileWriteBack(context, *recorder, htileAddress, StencilExpanded, 8u);
            static_cast<void>(Draw(*device, shaders, stencil, {}));
            Require(HtileStoreBackReads() == reads + 1u, "a stencil HTILE with a pending write was not read exactly");
        }
        Require(HtileSpeculationCounters().speculated == beforeStencil.speculated, "a stencil HTILE was speculated");
        std::puts("depth HTILE speculation tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
