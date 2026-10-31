#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// APS5_FLUSH_SKIP_SYNC (assigned by the test): a flush for a GPU reader skips the wait for
// the open batch that wrote the images only under the rule (Texture.hpp BlockerOf), and the
// stored bytes are the ones the wait would have given.

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::uint32_t Overwritten = 0x22222222u;
constexpr std::uint32_t CpuWritten = 0x33333333u;
constexpr std::size_t OverwrittenDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> StoreGlobalCode{0x7e000208u, 0x7e020209u, 0x7e0402ffu, Overwritten, 0xdc708100u, 0x007d0200u, 0xbf810000u};

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
        Require(block != nullptr, "storage flush skip: cannot allocate the guest block");
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

ShaderRecompiler::RecompileResult Compile(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::uint32_t> ImageUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (Type2D << 28u);
    return userData;
}

std::vector<std::uint32_t> GlobalUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(10, 0u);
    userData[8] = static_cast<std::uint32_t>(base);
    userData[9] = static_cast<std::uint32_t>(base >> 32u);
    return userData;
}

void StoreImage(AgcDriver::VulkanDevice& device, std::uint64_t image) {
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image), device.Target());
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
}

bool OpenWrite(std::uint64_t image) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    return recorder != nullptr && recorder->OpenWriteOverlaps(image, ImageBytes);
}

// Flushes the image for `reader` and returns whether the wait for the open batch was skipped.
bool Flush(std::uint64_t image, AgcDriver::Graphics::FlushReader reader, const std::string& what) {
    const auto before = StorageTexture::WriterWaitsSkipped();
    Require(StorageTexture::FlushPending(image, ImageBytes, nullptr, "test", AgcDriver::Graphics::PublishScope::Whole, nullptr, false, reader), what + "the image store left no results pending");
    return StorageTexture::WriterWaitsSkipped() != before;
}

// The image stored once into the open batch (no writer to wait for yet: the store is only
// recorded, its copies into the import an open write), then stored again: the next flush has the
// open batch as the writer of the range.
void StoreTwice(AgcDriver::VulkanDevice& device, std::uint64_t image, const std::string& what) {
    StoreImage(device, image);
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Gpu, what), what + "the first flush had a wait to skip");
    Require(OpenWrite(image), what + "the first store's copies are not an open write of the range");
    StoreImage(device, image);
}

// The rule over hand-made facts: each condition alone blocks the skip.
void RuleBlocksEachCondition() {
    using AgcDriver::Graphics::BlockerOf;
    using AgcDriver::Graphics::WriterWaitBlocker;
    using AgcDriver::Graphics::WriterWaitFacts;
    const WriterWaitFacts eligible{true, true, true, false, false, false};
    Require(BlockerOf(eligible) == WriterWaitBlocker::None, "storage flush skip rule: the eligible facts were blocked");
    auto facts = eligible;
    facts.stored = false;
    Require(BlockerOf(facts) == WriterWaitBlocker::NothingStored, "storage flush skip rule: nothing stored did not block");
    facts = eligible;
    facts.imported = false;
    Require(BlockerOf(facts) == WriterWaitBlocker::Import, "storage flush skip rule: (a) a span without an import did not block");
    facts = eligible;
    facts.watched = false;
    Require(BlockerOf(facts) == WriterWaitBlocker::Watch, "storage flush skip rule: (b) a span outside the write watch did not block");
    facts = eligible;
    facts.compared = true;
    Require(BlockerOf(facts) == WriterWaitBlocker::Compared, "storage flush skip rule: (d) a compared page did not block");
    facts = eligible;
    facts.cpuBlockOnOpenWrite = true;
    Require(BlockerOf(facts) == WriterWaitBlocker::CpuBlock, "storage flush skip rule: (c) a CPU-written block on an open write did not block");
    facts = eligible;
    facts.addressStore = true;
    Require(BlockerOf(facts) == WriterWaitBlocker::AddressStore, "storage flush skip rule: (e) an address store in the open batch did not block");
}

void GpuReaderSkips(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush skip, GPU reader: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    StoreTwice(device, image, what);
    Require(Flush(image, AgcDriver::Graphics::FlushReader::Gpu, what), what + "the wait for the open batch was not skipped");
    device.WaitIdle();
    Require(words[0] == Stored && words[FarDword] == Stored, what + "the image's texels were not stored");
}

void CpuReaderWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush skip, CPU reader: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    StoreTwice(device, image, what);
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Cpu, what), what + "a CPU reader's flush skipped the wait");
    device.WaitIdle();
    Require(words[FarDword] == Stored, what + "the image's texel was not stored");
}

void AddressStoreWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush skip, address store in the open batch: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    StoreImage(device, image);
    const auto storeGlobal = Compile(StoreGlobalCode, GlobalUserData(image), device.Target());
    device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreGlobalCode.data()));
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Gpu, what), what + "the flush skipped the wait over a batch storing through addresses");
    device.WaitIdle();
    Require(words[FarDword] == Stored, what + "the image's texel away from the newer write was not stored");
    Require(words[OverwrittenDword] == Overwritten, what + "the image's store put its older texels over a newer address-based store");
}

void CpuBlockOnOpenWriteWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush skip, CPU-written block on an open write: ";
    auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    StoreTwice(device, image, what);
    words[FarDword] = CpuWritten;
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Gpu, what), what + "the flush skipped the wait with a CPU-written block under an open write");
    device.WaitIdle();
}

}

int main() {
    try {
        RuleBlocksEachCondition();
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::FlushSkipSync, true);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::GuestMemory::WriteWatched()) {
            std::puts("guest memory has no write watch: storage flush skip tests skipped");
            return VulkanTestSkipped;
        }
        GuestBlock guest;
        GpuReaderSkips(*device, guest, 0);
        CpuReaderWaits(*device, guest, BlockBytes / 4u);
        AddressStoreWaits(*device, guest, BlockBytes / 2u);
        CpuBlockOnOpenWriteWaits(*device, guest, BlockBytes / 4u * 3u);
        std::puts("storage flush skip tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
