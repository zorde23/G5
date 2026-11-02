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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
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
constexpr std::uint32_t Second = 0x44444444u;
constexpr std::size_t OverwrittenDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> StoreSecondCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Second, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> StoreGlobalCode{0x7e000208u, 0x7e020209u, 0x7e0402ffu, Overwritten, 0xdc708100u, 0x007d0200u, 0xbf810000u};
constexpr std::size_t SlotBytes = ImageBytes;
constexpr std::size_t Attempts = BlockBytes / SlotBytes - 1;

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

void StoreSecond(AgcDriver::VulkanDevice& device, std::uint64_t image) {
    const auto storeSecond = Compile(StoreSecondCode, ImageUserData(image), device.Target());
    device.Dispatch(storeSecond, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreSecondCode.data()));
}

class SubmissionGate {
public:
    explicit SubmissionGate(AgcDriver::VulkanDevice& device) : device(device) {
#ifdef _WIN32
        constexpr const char* libraryName = "vulkan-1.dll";
#else
        constexpr const char* libraryName = "libvulkan.so.1";
#endif
        library.reset(SDL_LoadObject(libraryName));
        Require(library != nullptr, "storage flush skip: cannot load Vulkan for the submission gate");
        resolve = reinterpret_cast<PFN_vkGetDeviceProcAddr>(SDL_LoadFunction(library.get(), "vkGetDeviceProcAddr"));
        Require(resolve != nullptr, "storage flush skip: cannot resolve vkGetDeviceProcAddr");
        const auto createSemaphore = function<PFN_vkCreateSemaphore>("vkCreateSemaphore");
        destroySemaphore = function<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        signalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(resolve(device.Device(), "vkSignalSemaphoreKHR"));
        if (signalSemaphore == nullptr) signalSemaphore = function<PFN_vkSignalSemaphore>("vkSignalSemaphore");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        Require(createSemaphore(device.Device(), &info, nullptr, &semaphore) == VK_SUCCESS, "storage flush skip: cannot create the submission gate");
    }

    ~SubmissionGate() {
        if (releaser.joinable()) releaser.join();
        Release();
        try {
            device.WaitIdle();
        } catch (...) {
        }
        destroySemaphore(device.Device(), semaphore, nullptr);
    }

    SubmissionGate(const SubmissionGate&) = delete;
    SubmissionGate& operator=(const SubmissionGate&) = delete;

    void HoldNextSubmit() {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder != nullptr, "storage flush skip: no active recorder for the submission gate");
        recorder->Commands();
        recorder->HoldNextSubmit(semaphore, 1);
    }

    void ReleaseAfter(std::chrono::milliseconds delay) {
        releaser = std::thread([this, delay] {
            std::this_thread::sleep_for(delay);
            Release();
        });
    }

    void Release() {
        if (signaled.exchange(true)) return;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore;
        info.value = 1;
        releaseResult = signalSemaphore(device.Device(), &info);
    }

    void Finish() {
        if (releaser.joinable()) releaser.join();
        Release();
        Require(releaseResult == VK_SUCCESS, "storage flush skip: cannot release the submission gate");
        device.WaitIdle();
    }

    bool Released() const { return signaled.load(); }

private:
    template <typename TFunction>
    TFunction function(const char* name) const {
        const auto function = reinterpret_cast<TFunction>(resolve(device.Device(), name));
        Require(function != nullptr, std::string("storage flush skip: cannot resolve ") + name);
        return function;
    }

    AgcDriver::VulkanDevice& device;
    std::unique_ptr<void, decltype(&SDL_UnloadObject)> library{nullptr, &SDL_UnloadObject};
    PFN_vkGetDeviceProcAddr resolve = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkSignalSemaphore signalSemaphore = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::thread releaser;
    std::atomic<bool> signaled{false};
    VkResult releaseResult = VK_SUCCESS;
};

bool InFlightWriter(std::uint64_t image) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    if (recorder == nullptr) return false;
    const auto writer = recorder->PendingWriterSerial(image, ImageBytes);
    return writer != 0 && writer != recorder->OpenSerial();
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
    facts = eligible;
    facts.completionOverlap = true;
    Require(BlockerOf(facts) == WriterWaitBlocker::Completion, "storage flush skip rule: (f) a completion over the span did not block");
}

constexpr auto HeldFor = std::chrono::milliseconds(300);

template <class Prepare>
void HeldWriter(AgcDriver::VulkanDevice& device, SubmissionGate& gate, std::uint64_t image, const std::string& what, Prepare prepare) {
    StoreImage(device, image);
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Gpu, what), what + "the first flush had a wait to skip");
    Require(OpenWrite(image), what + "the first store's copies are not an open write of the range");
    StoreSecond(device, image);
    prepare();
    gate.HoldNextSubmit();
    device.SubmitRecorded(false);
    Require(InFlightWriter(image), what + "the held batch is not the in-flight writer of the range");
}

void RequireWaits(AgcDriver::VulkanDevice& device, SubmissionGate& gate, std::uint64_t image, const std::string& what) {
    gate.ReleaseAfter(HeldFor);
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Label, what), what + "the flush skipped the wait");
    Require(gate.Released(), what + "the flush returned before the held writer could finish");
    gate.Finish();
}

void LabelReaderSkipsInFlightWriter(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip, label reader with the writer in flight: ";
    SubmissionGate gate(device);
    const auto image = guest.Address() + SlotBytes;
    const auto* words = guest.Words() + SlotBytes / 4u;
    HeldWriter(device, gate, image, what, [] {});
    Require(Flush(image, AgcDriver::Graphics::FlushReader::Label, what), what + "a flush with the writer in flight and no blocker waited");
    Require(!gate.Released(), what + "the gate opened before the flush returned");
    std::vector<std::uint32_t> read(FarDword + 1u, 0u);
    std::thread reader([&] { AgcDriver::GuestMemory::Read(image, std::as_writable_bytes(std::span(read))); });
    gate.ReleaseAfter(HeldFor);
    reader.join();
    gate.Finish();
    Require(read[0] == Second && read[FarDword] == Second, what + "a CPU read after the skipped wait saw the bytes before the second store");
    Require(words[0] == Second && words[FarDword] == Second, what + "the image's second store was not stored");
}

void LabelReaderCpuBlockWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip, label reader with a CPU-written block under an in-flight write: ";
    SubmissionGate gate(device);
    const auto image = guest.Address() + 2 * SlotBytes;
    auto* words = guest.Words() + 2 * SlotBytes / 4u;
    HeldWriter(device, gate, image, what, [&] { words[FarDword] = CpuWritten; });
    RequireWaits(device, gate, image, what);
}

void LabelReaderCompletionWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip, label reader with a write-back completion in flight: ";
    SubmissionGate gate(device);
    const auto image = guest.Address() + 3 * SlotBytes;
    HeldWriter(device, gate, image, what, [] {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::Recorder::Active()->OnComplete([] {});
    });
    RequireWaits(device, gate, image, what);
}

void LabelReaderCompletionLabelWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip, label reader with a completion label on the span: ";
    SubmissionGate gate(device);
    const auto image = guest.Address() + 4 * SlotBytes;
    const std::uint32_t value = Stored;
    HeldWriter(device, gate, image, what, [&] {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::Recorder::Active()->AfterCompletions(image + 0x200u, std::as_bytes(std::span(&value, 1)), 1, 0, false);
    });
    Require(AgcDriver::Graphics::Recorder::PendingWriteBackCompletions() == 0, what + "a write-back completion is pending besides the completion label");
    RequireWaits(device, gate, image, what);
}

void LabelReaderOpenNoteOverInFlightAddressStore(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip, label reader with an open note over an in-flight address store: ";
    SubmissionGate gate(device);
    const auto image = guest.Address() + 5 * SlotBytes;
    HeldWriter(device, gate, image, what, [&] {
        const auto storeGlobal = Compile(StoreGlobalCode, GlobalUserData(image), device.Target());
        device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreGlobalCode.data()));
    });
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        const std::uint32_t value = Stored;
        recorder->NoteLabel(image + 0x300u, std::as_bytes(std::span(&value, 1)), 1, 0);
        Require(recorder->PendingWriterSerial(image, ImageBytes) == recorder->OpenSerial(), what + "the open batch is not the newest pending writer of the range");
    }
    RequireWaits(device, gate, image, what);
}

void LabelReaderAfterInFlightRead(AgcDriver::VulkanDevice& device, const GuestBlock& guest, const GuestBlock& results) {
    const std::string what = "storage flush skip, label reader after an in-flight read of the range: ";
    SubmissionGate gate(device);
    const auto offset = 6 * SlotBytes;
    const auto image = guest.Address() + offset;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        gate.HoldNextSubmit();
        const auto outcome = device.CopyBuffer(results.Address() + offset, image, ImageBytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, 0, [](std::span<const std::byte>, std::uint64_t) {});
        Require(outcome.path == 1, what + "the copy was not a transfer between the host imports (path " + std::to_string(outcome.path) + ")");
    }
    device.SubmitRecorded(false);
    StoreImage(device, image);
    Flush(image, AgcDriver::Graphics::FlushReader::Label, what);
    Require(!gate.Released(), what + "the flush waited for the reading batch");
    gate.Finish();
    Require(results.Words()[offset / 4u] == Initial, what + "the in-flight read saw the image's store instead of the bytes before it");
    Require(guest.Words()[offset / 4u] == Stored, what + "the image's texel was not stored");
}

void LabelVisibleAfterImage(AgcDriver::VulkanDevice& device, const GuestBlock& guest, const GuestBlock& labels) {
    const std::string what = "storage flush skip, label after a readback that skipped an in-flight writer: ";
    SubmissionGate gate(device);
    const auto image = guest.Address() + 7 * SlotBytes;
    const auto* words = static_cast<volatile const std::uint32_t*>(guest.Words() + 7 * SlotBytes / 4u);
    auto* label = static_cast<volatile std::uint32_t*>(labels.Words());
    const std::uint32_t value = 0x1abe1u;
    HeldWriter(device, gate, image, what, [] {});
    int reason = 4;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto before = StorageTexture::WriterWaitsSkipped();
        reason = device.WriteLabelOnGpu(labels.Address(), std::as_bytes(std::span(&value, 1)), 1, 0);
        Require(StorageTexture::WriterWaitsSkipped() != before, what + "the label's readback did not skip the wait for the held writer");
        Require(reason == 0 || reason == 5, what + "the label was not ordered behind the write-back (reason " + std::to_string(reason) + ")");
        device.SubmitRecorded(true);
    }
    Require(*label != value, what + "the label landed while its writer was held");
    gate.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (*label != value) {
        Require(std::chrono::steady_clock::now() < deadline, what + "the label never landed");
        std::this_thread::yield();
    }
    Require(words[0] == Second && words[FarDword] == Second, what + "the label was visible before the image's second store");
    gate.Finish();
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
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::FlushSkipSyncLabel, true);
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
        {
            GuestBlock images;
            GuestBlock results;
            LabelReaderSkipsInFlightWriter(*device, images);
            LabelReaderCpuBlockWaits(*device, images);
            LabelReaderCompletionWaits(*device, images);
            LabelReaderCompletionLabelWaits(*device, images);
            LabelReaderOpenNoteOverInFlightAddressStore(*device, images);
            LabelReaderAfterInFlightRead(*device, images, results);
            LabelVisibleAfterImage(*device, images, results);
        }
        std::puts("storage flush skip tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
