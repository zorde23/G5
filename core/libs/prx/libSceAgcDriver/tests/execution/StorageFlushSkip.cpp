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
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
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

void LabelReaderLaterCompletionGap(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip, label reader with a completion that may write the span in a batch after the writer: ";
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::FlushSkipSyncLabel, false);
    SubmissionGate gate(device);
    const auto image = guest.Address() + SlotBytes;
    HeldWriter(device, gate, image, what, [] {});
    std::atomic<bool> ran{false};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        recorder->OnComplete([&ran] { ran = true; });
        Require(recorder->PendingWriterSerial(image, ImageBytes) != recorder->OpenSerial(), what + "the batch holding the completion became the writer of the range");
    }
    gate.ReleaseAfter(HeldFor);
    Require(!Flush(image, AgcDriver::Graphics::FlushReader::Label, what), what + "the flush skipped the wait");
    const bool pendingAtStore = !ran.load();
    gate.Finish();
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::FlushSkipSyncLabel, true);
    Require(ran.load(), what + "the completion never ran");
    Require(pendingAtStore, what + "the known gap changed (the readback now waits for a later batch's completion); update this test and its debt entry");
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


using AgcDriver::Graphics::FlushReader;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::RuntimeSwitch;
namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;

constexpr std::uint32_t Stale = 0x55555555u;
constexpr std::size_t CaseBytes = 2 * SlotBytes;
constexpr std::size_t UnitBytes = 65536;
constexpr auto SkipGrace = std::chrono::milliseconds(3000);

Recorder& ActiveRecorder() {
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "storage flush skip narrow: no active recorder");
    return *recorder;
}

void SetNarrow(bool on) {
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelNarrow, on);
}

std::function<void()> StoreWord(std::uint64_t address, std::uint32_t value, std::atomic<bool>* ran = nullptr) {
    return [address, value, ran] {
        if (ran != nullptr) ran->store(true);
        AgcDriver::GuestMemory::StoreOwnBytes(address, std::as_bytes(std::span(&value, 1)));
    };
}

Recorder::CompletionWrites Declared(std::uint64_t begin, std::uint64_t end, bool hookable = false) {
    return {true, {{begin, end}}, hookable};
}

void SyncAll() {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    ActiveRecorder().Sync();
}

std::vector<std::uint32_t> CaseWords(const GuestBlock& guest, std::size_t caseOffset) {
    const auto* words = guest.Words() + caseOffset / 4u;
    return {words, words + CaseBytes / 4u};
}

enum class Expect : std::uint8_t { Wait, Skip };

struct Hooks {
    std::function<void(std::uint64_t base, std::uint64_t image)> prepare;
    std::function<void(Recorder&, std::uint64_t base, std::uint64_t image)> beforeSubmit;
    std::function<void(Recorder&, std::uint64_t base, std::uint64_t image, SubmissionGate&)> afterSubmit;
};

struct Outcome {
    bool skipped = false;
    bool ranAtFlush = false;
    std::vector<std::uint32_t> words;
};

Outcome RunCase(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t caseOffset, std::size_t imageOffset, bool oracle, Expect expect, const std::string& what, std::atomic<bool>& ran, const Hooks& hooks) {
    SubmissionGate gate(device);
    const auto base = guest.Address() + caseOffset;
    const auto image = base + imageOffset;
    ran = false;
    HeldWriter(device, gate, image, what, [&] {
        if (hooks.prepare) hooks.prepare(base, image);
        if (!hooks.beforeSubmit) return;
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        hooks.beforeSubmit(ActiveRecorder(), base, image);
    });
    if (hooks.afterSubmit) {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        hooks.afterSubmit(ActiveRecorder(), base, image, gate);
    }
    if (oracle) {
        gate.Release();
        SyncAll();
    } else {
        gate.ReleaseAfter(expect == Expect::Wait ? HeldFor : SkipGrace);
    }
    Outcome outcome;
    outcome.skipped = Flush(image, FlushReader::Label, what);
    outcome.ranAtFlush = ran.load();
    const bool held = !gate.Released();
    if (!oracle && expect == Expect::Wait) {
        Require(!outcome.skipped, what + "the flush skipped the wait");
        Require(outcome.ranAtFlush, what + "the results were stored before the conflicting completion ran");
    }
    if (!oracle && expect == Expect::Skip) {
        Require(outcome.skipped && held, what + "the flush waited for the held writer");
        Require(!outcome.ranAtFlush, what + "the completion ran before the skipped flush returned");
    }
    gate.Finish();
    SyncAll();
    outcome.words = CaseWords(guest, caseOffset);
    return outcome;
}

void RequireSameBytes(const Outcome& narrow, const Outcome& oracle, const std::string& what) {
    for (std::size_t index = 0; index < narrow.words.size(); ++index) {
        if (narrow.words[index] == oracle.words[index]) continue;
        char text[160];
        std::snprintf(text, sizeof(text), "the byte at case offset 0x%zx is %08x, the settled order gives %08x", index * 4u, narrow.words[index], oracle.words[index]);
        Require(false, what + text);
    }
}

void CheckCase(AgcDriver::VulkanDevice& device, const std::string& what, std::size_t imageOffset, Expect expect, std::atomic<bool>& ran, const Hooks& hooks) {
    GuestBlock guest;
    SetNarrow(true);
    const auto narrow = RunCase(device, guest, 0, imageOffset, false, expect, what, ran, hooks);
    const auto oracle = RunCase(device, guest, BlockBytes / 2u, imageOffset, true, expect, what + "(settled) ", ran, hooks);
    SetNarrow(false);
    RequireSameBytes(narrow, oracle, what);
}

void FlushImage(std::uint64_t image) {
    static_cast<void>(StorageTexture::FlushPending(image, ImageBytes, nullptr, "test", AgcDriver::Graphics::PublishScope::Whole, nullptr, false, FlushReader::Gpu));
    SyncAll();
}

void NarrowAuditCountsOutsideStores(const GuestBlock& guest) {
    const std::string what = "storage flush skip narrow, completion auditor: ";
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelShadow, true);
    static_cast<void>(Recorder::TakeCompletionAudit());
    const auto base = guest.Address();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        recorder.OnComplete(StoreWord(base + 0x100u, Stale), Declared(base + 0x100u, base + 0x104u));
        recorder.Sync();
    }
    auto audit = Recorder::TakeCompletionAudit();
    Require(audit.completions >= 1 && audit.outsideStores == 0 && audit.unknownStores == 0, what + "a store inside the declared range was counted");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        recorder.OnComplete(StoreWord(base + 0x200u, Stale), Declared(base + 0x100u, base + 0x104u));
        recorder.OnComplete(StoreWord(base + 0x300u, Stale));
        recorder.Sync();
    }
    audit = Recorder::TakeCompletionAudit();
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelShadow, false);
    Require(audit.outsideStores == 1 && audit.outsideBytes == 4, what + "the store outside the declared range was not counted once");
    Require(audit.unknownStores == 1, what + "the undeclared completion's store was not counted");
    Require(guest.Words()[0x200u / 4u] == Stale, what + "the counted store did not land");
}

void NarrowAuditVerifyFails() {
    GuestBlock guest;
    const auto base = guest.Address();
    static_cast<void>(Recorder::TakeCompletionAudit());
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        recorder.OnComplete(StoreWord(base + 0x100u, Stale), Declared(base + 0x100u, base + 0x104u));
        recorder.OnComplete(StoreWord(base + 0x200u, Stale), Declared(base + 0x100u, base + 0x104u));
        recorder.Sync();
    }
    const auto audit = Recorder::TakeCompletionAudit();
    Require(guest.Words()[0x100u / 4u] == Stale, "storage flush skip narrow verify: the store inside the declared range did not land");
    Require(guest.Words()[0x200u / 4u] == Initial, "storage flush skip narrow verify: the store outside the declared range landed");
    Require(audit.outsideStores == 1, "storage flush skip narrow verify: the store outside the declared range was not counted");
}

void NarrowHtileSplitWaits(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, HTILE split over two batches: ";
    std::atomic<bool> ran{false};
    Hooks hooks;
    hooks.afterSubmit = [&](Recorder& recorder, std::uint64_t, std::uint64_t image, SubmissionGate& gate) {
        recorder.Commands();
        recorder.NotePendingWrite(image, ImageBytes);
        recorder.Submit();
        const auto first = recorder.OpenSerial() - 1;
        recorder.Commands();
        recorder.NotePendingWrite(image + ImageBytes, ImageBytes);
        Require(recorder.AfterPendingWrite(image, 2 * ImageBytes, StoreWord(image + 0x40u, Stale, &ran)), what + "the speculation found no batch");
        gate.Release();
        recorder.FinishUpTo(first);
        Require(recorder.PendingWriterSerial(image, ImageBytes) == 0, what + "a batch still writes the image after the first half's batch finished");
    };
    CheckCase(device, what, 0, Expect::Wait, ran, hooks);
}

void NarrowSharedUnitWaits(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, completion in the image's partial unit: ";
    std::atomic<bool> ran{false};
    Hooks hooks;
    hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t base, std::uint64_t) { recorder.OnComplete(StoreWord(base + 0x100u, Stale, &ran), Declared(base + 0x100u, base + 0x104u)); };
    CheckCase(device, what, UnitBytes / 2u, Expect::Wait, ran, hooks);
}

void NarrowLaterCompletionsWait(AgcDriver::VulkanDevice& device) {
    for (int variant = 0; variant < 3; ++variant) {
        const std::string what = std::string("storage flush skip narrow, completion over the span ") + (variant == 0 ? "in the open batch: " : variant == 1 ? "in a batch after the writer: " : "with the writer retired: ");
        std::atomic<bool> ran{false};
        Hooks hooks;
        hooks.afterSubmit = [&, variant](Recorder& recorder, std::uint64_t, std::uint64_t image, SubmissionGate& gate) {
            if (variant == 2) {
                const auto writer = recorder.PendingWriterSerial(image, ImageBytes);
                gate.Release();
                recorder.FinishUpTo(writer);
            }
            recorder.Commands();
            recorder.OnComplete(StoreWord(image + 0x40u, Stale, &ran), Declared(image + 0x40u, image + 0x44u));
            if (variant == 1) recorder.Submit();
            if (variant == 2) Require(recorder.PendingWriterSerial(image, ImageBytes) == 0, what + "the writer did not retire");
        };
        CheckCase(device, what, 0, Expect::Wait, ran, hooks);
    }
}

void NarrowOutsideCompletionSkips(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, completion outside the span's units: ";
    std::atomic<bool> ran{false};
    Hooks hooks;
    hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t base, std::uint64_t) { recorder.OnComplete(StoreWord(base + 3u * UnitBytes, Stale, &ran), Declared(base + 3u * UnitBytes, base + 3u * UnitBytes + 4u)); };
    CheckCase(device, what, 0, Expect::Skip, ran, hooks);
    GuestBlock global;
    RunCase(device, global, 0, 0, false, Expect::Wait, "storage flush skip, global rule with a completion outside the span: ", ran, hooks);
}

void NarrowCopiedWriterWaits(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, copied writer over the span: ";
    std::atomic<bool> ran{false};
    Hooks hooks;
    hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t, std::uint64_t image) { recorder.OnComplete(StoreWord(image + 0x40u, Stale, &ran), Declared(image, image + ImageBytes, true)); };
    CheckCase(device, what, 0, Expect::Wait, ran, hooks);
}

alignas(256) constexpr std::array<std::uint32_t, 8> LoadGlobalCode{0x7e000208u, 0x7e020209u, 0xdc308100u, 0x037d0000u, 0xbf8c3f70u, 0xbf810000u, 0xbf810000u, 0xbf810000u};

void NarrowAddressReadSkips(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, address-based read in the open batch: ";
    GuestBlock guest;
    SetNarrow(true);
    SubmissionGate gate(device);
    const auto image = guest.Address();
    HeldWriter(device, gate, image, what, [] {});
    const auto loadGlobal = Compile(LoadGlobalCode, GlobalUserData(image), device.Target());
    device.Dispatch(loadGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(LoadGlobalCode.data()));
    gate.ReleaseAfter(SkipGrace);
    const bool skipped = Flush(image, FlushReader::Label, what);
    const bool held = !gate.Released();
    gate.Finish();
    SyncAll();
    SetNarrow(false);
    Require(skipped && held, what + "the flush waited");
    Require(guest.Words()[0] == Second && guest.Words()[FarDword] == Second, what + "the image's second store was not stored");
    GuestBlock stores;
    SetNarrow(true);
    LabelReaderOpenNoteOverInFlightAddressStore(device, stores);
    SetNarrow(false);
}

void NarrowUnitEdges(AgcDriver::VulkanDevice& device) {
    const auto imageOffset = UnitBytes / 2u;
    const auto spanEnd = (imageOffset + ImageBytes + UnitBytes - 1u) & ~(UnitBytes - 1u);
    {
        const std::string what = "storage flush skip narrow, completion just before the image in its first unit: ";
        std::atomic<bool> ran{false};
        Hooks hooks;
        hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t, std::uint64_t image) { recorder.OnComplete(StoreWord(image - 4u, Stale, &ran), Declared(image - 4u, image)); };
        CheckCase(device, what, imageOffset, Expect::Wait, ran, hooks);
    }
    {
        const std::string what = "storage flush skip narrow, completion just after the image's last unit: ";
        std::atomic<bool> ran{false};
        Hooks hooks;
        hooks.beforeSubmit = [&, spanEnd](Recorder& recorder, std::uint64_t base, std::uint64_t) { recorder.OnComplete(StoreWord(base + spanEnd, Stale, &ran), Declared(base + spanEnd, base + spanEnd + 4u)); };
        CheckCase(device, what, imageOffset, Expect::Skip, ran, hooks);
    }
    {
        const std::string what = "storage flush skip narrow, completion just inside the image's last unit: ";
        std::atomic<bool> ran{false};
        Hooks hooks;
        hooks.beforeSubmit = [&, spanEnd](Recorder& recorder, std::uint64_t base, std::uint64_t) { recorder.OnComplete(StoreWord(base + spanEnd - 4u, Stale, &ran), Declared(base + spanEnd - 4u, base + spanEnd)); };
        CheckCase(device, what, imageOffset, Expect::Wait, ran, hooks);
    }
}

void NarrowUndeclaredWaits(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, undeclared completion: ";
    std::atomic<bool> ran{false};
    Hooks hooks;
    hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t base, std::uint64_t) { recorder.OnComplete(StoreWord(base + 3u * UnitBytes, Stale, &ran)); };
    CheckCase(device, what, 0, Expect::Wait, ran, hooks);
}

void NarrowInsideCompletion(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, label readback inside a completion: ";
    GuestBlock guest;
    SetNarrow(true);
    SubmissionGate gate(device);
    const auto image = guest.Address();
    HeldWriter(device, gate, image, what, [] {});
    const auto conflictWaits = StorageTexture::NarrowConflictWaits();
    std::atomic<bool> inside{false};
    std::atomic<bool> flushed{false};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.OnComplete(StoreWord(image + 0x40u, Stale), Declared(image + 0x40u, image + 0x44u));
        recorder.OnComplete([&, image] {
            inside = Recorder::InCompletion();
            flushed = StorageTexture::FlushPending(image, ImageBytes, nullptr, "test", AgcDriver::Graphics::PublishScope::Whole, nullptr, false, FlushReader::Label);
        }, {true, {}, false});
    }
    gate.Release();
    SyncAll();
    gate.Finish();
    SyncAll();
    SetNarrow(false);
    Require(inside.load() && flushed.load(), what + "the readback did not run inside the completion");
    Require(StorageTexture::NarrowConflictWaits() == conflictWaits, what + "the readback inside the completion waited for a conflict");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(ActiveRecorder().Seals() == 0, what + "a seal outlived the batches");
    }
}

void NarrowLabelVisibleAfterImage(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, label after a readback that skipped: ";
    GuestBlock guest;
    GuestBlock labels;
    SetNarrow(true);
    SubmissionGate gate(device);
    const auto image = guest.Address();
    const auto* words = static_cast<volatile const std::uint32_t*>(guest.Words());
    auto* label = static_cast<volatile std::uint32_t*>(labels.Words());
    const std::uint32_t value = 0x1abe2u;
    HeldWriter(device, gate, image, what, [&] {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        ActiveRecorder().OnComplete(StoreWord(image + 3u * UnitBytes, Stale), Declared(image + 3u * UnitBytes, image + 3u * UnitBytes + 4u));
    });
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto before = StorageTexture::WriterWaitsSkipped();
        const auto reason = device.WriteLabelOnGpu(labels.Address(), std::as_bytes(std::span(&value, 1)), 1, 0);
        Require(StorageTexture::WriterWaitsSkipped() != before, what + "the label's readback did not skip the wait");
        Require(reason == 0 || reason == 5, what + "the label was not ordered behind the write-back (reason " + std::to_string(reason) + ")");
        Require(ActiveRecorder().Seals() != 0, what + "the skip left no seal");
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
    SyncAll();
    SetNarrow(false);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Require(ActiveRecorder().Seals() == 0, what + "the seal outlived its batch");
}

void NarrowLaterCompletionClosesGap(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush skip narrow, completion in a batch after the writer: ";
    SetNarrow(true);
    SubmissionGate gate(device);
    const auto image = guest.Address() + 2 * SlotBytes;
    HeldWriter(device, gate, image, what, [] {});
    std::atomic<bool> ran{false};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.OnComplete([&ran] { ran = true; });
        Require(recorder.PendingWriterSerial(image, ImageBytes) != recorder.OpenSerial(), what + "the batch holding the completion became the writer of the range");
    }
    gate.ReleaseAfter(HeldFor);
    Require(!Flush(image, FlushReader::Label, what), what + "the flush skipped the wait");
    const bool ranAtStore = ran.load();
    gate.Finish();
    SetNarrow(false);
    Require(ranAtStore, what + "the results were stored before the later batch's completion ran");
}

void NarrowHookableWriteBack(AgcDriver::VulkanDevice& device) {
    for (const bool dirty : {true, false}) {
        const std::string what = std::string("storage flush skip narrow, write-back with ") + (dirty ? "pending results" : "no pending results") + " in its declared range: ";
        std::atomic<bool> ran{false};
        Hooks hooks;
        hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t base, std::uint64_t) {
            const auto other = base + SlotBytes;
            recorder.OnComplete(StoreWord(other + 0x40u, Stale, &ran), Declared(other, other + ImageBytes, true));
        };
        GuestBlock guest;
        SetNarrow(true);
        for (const std::size_t caseOffset : {std::size_t{0}, BlockBytes / 2u}) {
            const auto other = guest.Address() + caseOffset + SlotBytes;
            StoreImage(device, other);
            if (!dirty) FlushImage(other);
        }
        const auto narrow = RunCase(device, guest, 0, 0, false, dirty ? Expect::Wait : Expect::Skip, what, ran, hooks);
        const auto oracle = RunCase(device, guest, BlockBytes / 2u, 0, true, Expect::Wait, what + "(settled) ", ran, hooks);
        SetNarrow(false);
        for (const std::size_t caseOffset : {std::size_t{0}, BlockBytes / 2u}) FlushImage(guest.Address() + caseOffset + SlotBytes);
        RequireSameBytes(narrow, oracle, what);
    }
    const std::string what = "storage flush skip narrow, auditor across a nested finish: ";
    GuestBlock guest;
    const auto base = guest.Address();
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelShadow, true);
    static_cast<void>(Recorder::TakeCompletionAudit());
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        const auto later = recorder.OpenSerial() + 1;
        recorder.OnComplete([&recorder, later, base] {
            recorder.FinishUpTo(later);
            const std::uint32_t value = Stale;
            AgcDriver::GuestMemory::StoreOwnBytes(base + 0x100u, std::as_bytes(std::span(&value, 1)));
        }, Declared(base + 0x100u, base + 0x104u));
        recorder.Submit();
        recorder.Commands();
        recorder.OnComplete([base] {
            const std::uint32_t value = Stale;
            AgcDriver::GuestMemory::StoreOwnBytes(base + 0x200u, std::as_bytes(std::span(&value, 1)));
            throw std::runtime_error("deliberate failure of a nested completion");
        }, Declared(base + 0x200u, base + 0x204u));
        recorder.Submit();
        recorder.Sync();
    }
    const auto audit = Recorder::TakeCompletionAudit();
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelShadow, false);
    Require(guest.Words()[0x100u / 4u] == Stale && guest.Words()[0x200u / 4u] == Stale, what + "a completion's store did not land");
    Require(audit.outsideStores == 0 && audit.unknownStores == 0, what + "the outer completion's store was audited against the nested one's ranges");
}

void NarrowSealFiresOnOlderCompletion(const GuestBlock& guest) {
    const std::string what = "storage flush skip narrow, seal: ";
    const auto base = guest.Address();
    const auto violations = Recorder::SealViolations();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        recorder.OnComplete(StoreWord(base + 0x100u, Stale), Declared(base + 0x100u, base + 0x104u));
        recorder.Submit();
        recorder.Commands();
        recorder.Seal(base, base + UnitBytes, recorder.OpenSerial());
        recorder.OnComplete(StoreWord(base + 0x200u, Stale), Declared(base + 0x200u, base + 0x204u));
        recorder.Sync();
        Require(recorder.Seals() == 0, what + "the seal outlived its batch");
    }
    Require(Recorder::SealViolations() == violations + 1, what + "an older batch's completion stored into the sealed span unnoticed");
    Require(guest.Words()[0x100u / 4u] == Initial, what + "the older batch's store into the sealed span landed");
    Require(guest.Words()[0x200u / 4u] == Stale, what + "the sealing batch's own completion could not store");
}

void NarrowLabelBehindCompletions(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, label behind the completions after a skip: ";
    GuestBlock guest;
    GuestBlock labels;
    SetNarrow(true);
    SubmissionGate gate(device);
    const auto image = guest.Address();
    const auto* words = static_cast<volatile const std::uint32_t*>(guest.Words());
    auto* label = static_cast<volatile std::uint32_t*>(labels.Words());
    const std::uint32_t value = 0x1abe3u;
    HeldWriter(device, gate, image, what, [] {});
    std::atomic<std::uint32_t> seenLabel{0}, seenFirst{0}, seenFar{0};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        const std::uint32_t other = 0x0dd5u;
        recorder.AfterCompletions(labels.Address() + 0x100u, std::as_bytes(std::span(&other, 1)), 1, 0, false);
        Require(Recorder::PendingCompletionStores() != 0, what + "no completion store is pending");
        const auto before = StorageTexture::WriterWaitsSkipped();
        const auto reason = device.WriteLabelOnGpu(labels.Address(), std::as_bytes(std::span(&value, 1)), 2, 0);
        Require(StorageTexture::WriterWaitsSkipped() != before, what + "the label's readback did not skip the wait");
        Require(reason == 5, what + "the label was not stored behind the completions (reason " + std::to_string(reason) + ")");
        recorder.OnComplete([&] {
            seenLabel = *label;
            seenFirst = words[0];
            seenFar = words[FarDword];
        }, {true, {}, false});
        device.SubmitRecorded(true);
    }
    Require(*label != value, what + "the label landed while its writer was held");
    gate.Finish();
    SyncAll();
    SetNarrow(false);
    Require(*label == value, what + "the label never landed");
    Require(seenLabel.load() == value, what + "the label had not landed when its batch's later completion ran");
    Require(seenFirst.load() == Second && seenFar.load() == Second, what + "the label landed before the image's second store");
}

void NarrowTwoLabelsSameSpan(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, two labels over one span: ";
    GuestBlock guest;
    GuestBlock labels;
    SetNarrow(true);
    SubmissionGate gate(device);
    const auto image = guest.Address();
    const auto* words = static_cast<volatile const std::uint32_t*>(guest.Words());
    auto* second = static_cast<volatile std::uint32_t*>(labels.Words() + 1);
    const std::uint32_t firstValue = 0x1abe4u;
    const std::uint32_t secondValue = 0x1abe5u;
    HeldWriter(device, gate, image, what, [] {});
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto before = StorageTexture::WriterWaitsSkipped();
        const auto reason = device.WriteLabelOnGpu(labels.Address(), std::as_bytes(std::span(&firstValue, 1)), 3, 0);
        Require(StorageTexture::WriterWaitsSkipped() != before, what + "the first label's readback did not skip the wait");
        Require(reason == 0 || reason == 5, what + "the first label was not ordered behind the write-back (reason " + std::to_string(reason) + ")");
    }
    StoreImage(device, image);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto before = StorageTexture::WriterWaitsSkipped();
        const auto reason = device.WriteLabelOnGpu(labels.Address() + 4u, std::as_bytes(std::span(&secondValue, 1)), 4, 0);
        Require(StorageTexture::WriterWaitsSkipped() != before, what + "the second label's readback did not skip the wait");
        Require(reason == 0 || reason == 5, what + "the second label was not ordered behind the write-back (reason " + std::to_string(reason) + ")");
        device.SubmitRecorded(true);
    }
    Require(!gate.Released(), what + "a readback waited for the held writer");
    Require(*second != secondValue, what + "the second label landed while the writer was held");
    gate.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (*second != secondValue) {
        Require(std::chrono::steady_clock::now() < deadline, what + "the second label never landed");
        std::this_thread::yield();
    }
    Require(words[0] == Stored && words[FarDword] == Stored, what + "the second label was visible before the third store");
    gate.Finish();
    SyncAll();
    SetNarrow(false);
}

void NarrowThrowingCompletions(AgcDriver::VulkanDevice& device) {
    {
        const std::string what = "storage flush skip narrow, conflicting completion that throws: ";
        GuestBlock guest;
        std::atomic<bool> ran{false};
        Hooks hooks;
        hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t, std::uint64_t image) {
            recorder.OnComplete([&ran] {
                ran = true;
                throw std::runtime_error("deliberate failure of a conflicting completion");
            }, Declared(image, image + 4u));
        };
        GuestBlock labels;
        const std::uint32_t value = 0x1abe6u;
        hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t, std::uint64_t image) {
            recorder.OnComplete([&ran] {
                ran = true;
                throw std::runtime_error("deliberate failure of a conflicting completion");
            }, Declared(image, image + 4u));
            recorder.AfterCompletions(labels.Address(), std::as_bytes(std::span(&value, 1)), 5, 0, false);
        };
        SetNarrow(true);
        RunCase(device, guest, 0, 0, false, Expect::Wait, what, ran, hooks);
        SetNarrow(false);
        Require(labels.Words()[0] != value, what + "the batch's label landed as a success after its completion failed");
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(ActiveRecorder().Seals() == 0, what + "a seal outlived the batches");
    }
    {
        const std::string what = "storage flush skip narrow, sealing batch whose completion throws: ";
        GuestBlock guest;
        GuestBlock labels;
        const std::uint32_t value = 0x1abe7u;
        SetNarrow(true);
        SubmissionGate gate(device);
        const auto image = guest.Address();
        HeldWriter(device, gate, image, what, [] {});
        gate.ReleaseAfter(SkipGrace);
        Require(Flush(image, FlushReader::Label, what) && !gate.Released(), what + "the flush did not skip");
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            auto& recorder = ActiveRecorder();
            Require(recorder.Seals() == 1, what + "the skip left no seal");
            recorder.OnComplete([] { throw std::runtime_error("deliberate failure of the sealing batch's completion"); }, {true, {}, false});
            recorder.AfterCompletions(labels.Address(), std::as_bytes(std::span(&value, 1)), 6, 0, false);
        }
        gate.Finish();
        SyncAll();
        SetNarrow(false);
        Require(labels.Words()[0] != value, what + "the sealing batch's label landed as a success after its completion failed");
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(ActiveRecorder().Seals() == 0, what + "the seal outlived its batch");
    }
}

void NarrowSealSurvivesNestedRetirement(const GuestBlock& guest, const GuestBlock& labels) {
    const std::string what = "storage flush skip narrow, seal across a nested retirement of its batch: ";
    const auto base = guest.Address();
    const auto violations = Recorder::SealViolations();
    const std::uint32_t value = 0x1abe8u;
    SetNarrow(true);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        const auto sealing = recorder.OpenSerial() + 1;
        recorder.OnComplete([&recorder, sealing, base] {
            recorder.FinishUpTo(sealing);
            Require(recorder.Seals() == 1, "the seal was dropped while an older batch's completion still runs");
            const std::uint32_t stale = Stale;
            AgcDriver::GuestMemory::StoreOwnBytes(base + 0x100u, std::as_bytes(std::span(&stale, 1)));
        }, Declared(base + 0x100u, base + 0x104u));
        recorder.AfterCompletions(labels.Address(), std::as_bytes(std::span(&value, 1)), 7, 0, false);
        recorder.Submit();
        recorder.Commands();
        recorder.Seal(base, base + UnitBytes, recorder.OpenSerial());
        recorder.OnComplete(StoreWord(base + 0x200u, Stale), Declared(base + 0x200u, base + 0x204u));
        recorder.Submit();
        recorder.Sync();
        Require(recorder.Seals() == 0, what + "the seal outlived the older batch");
    }
    SetNarrow(false);
    Require(Recorder::SealViolations() == violations + 1, what + "the older completion stored into the sealed span after the nested retirement unnoticed");
    Require(guest.Words()[0x100u / 4u] == Initial, what + "the older completion's store into the sealed span landed");
    Require(guest.Words()[0x200u / 4u] == Stale, what + "the sealing batch's own completion could not store");
    Require(labels.Words()[0] != value, what + "the older batch's label landed as a success after its seal violation");
}

void NarrowAuditUnionOfRanges(const GuestBlock& guest) {
    const std::string what = "storage flush skip narrow, auditor over overlapping declarations: ";
    const auto base = guest.Address();
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelShadow, true);
    static_cast<void>(Recorder::TakeCompletionAudit());
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto& recorder = ActiveRecorder();
        recorder.Commands();
        recorder.OnComplete([base] {
            const std::array<std::uint32_t, 3> words{Stale, Stale, Stale};
            AgcDriver::GuestMemory::StoreOwnBytes(base + 0x400u, std::as_bytes(std::span(words)));
        }, {true, {{base + 0x404u, base + 0x40cu}, {base + 0x400u, base + 0x408u}}, true});
        recorder.OnComplete([base] {
            const std::array<std::uint32_t, 3> words{Stale, Stale, Stale};
            AgcDriver::GuestMemory::StoreOwnBytes(base + 0x500u, std::as_bytes(std::span(words)));
        }, {true, {{base + 0x500u, base + 0x504u}, {base + 0x508u, base + 0x50cu}}, true});
        recorder.Sync();
    }
    const auto audit = Recorder::TakeCompletionAudit();
    RuntimeSwitches::Assign(RuntimeSwitch::FlushSkipSyncLabelShadow, false);
    Require(audit.outsideStores == 1, what + "a store covered by the union of overlapping ranges was counted, or one across a gap was not");
}

void NarrowCommitterBindsDuringDecisions(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, committer binding during decisions: ";
    GuestBlock guest;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> toggles{0};
    std::thread toggler([&] {
        while (!stop.load()) {
            Recorder::BindCommitter();
            std::this_thread::yield();
            Recorder::UnbindCommitter();
            toggles.fetch_add(1);
        }
    });
    SetNarrow(true);
    std::exception_ptr failure;
    try {
        for (std::size_t round = 0; round < 32; ++round) {
            const auto image = guest.Address() + (round % 4u) * 2u * SlotBytes;
            StoreImage(device, image);
            Flush(image, FlushReader::Label, what);
        }
    } catch (...) {
        failure = std::current_exception();
    }
    stop = true;
    toggler.join();
    SetNarrow(false);
    SyncAll();
    if (failure) std::rethrow_exception(failure);
    Require(toggles.load() != 0, what + "the committer was never bound during the decisions");
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Require(Recorder::OnCommitter(), what + "a committer stayed bound after the toggling thread ended");
}

void NarrowRejectsOffCommitter(AgcDriver::VulkanDevice& device) {
    const std::string what = "storage flush skip narrow, decision off the committer: ";
    std::thread([] { Recorder::BindCommitter(); }).join();
    const auto rejections = Recorder::CommitterRejections();
    std::atomic<bool> ran{false};
    Hooks hooks;
    hooks.beforeSubmit = [&](Recorder& recorder, std::uint64_t base, std::uint64_t) { recorder.OnComplete(StoreWord(base + 3u * UnitBytes, Stale, &ran), Declared(base + 3u * UnitBytes, base + 3u * UnitBytes + 4u)); };
    GuestBlock guest;
    SetNarrow(true);
    try {
        RunCase(device, guest, 0, 0, false, Expect::Wait, what, ran, hooks);
    } catch (...) {
        Recorder::BindCommitter();
        Recorder::UnbindCommitter();
        SetNarrow(false);
        throw;
    }
    Recorder::BindCommitter();
    Recorder::UnbindCommitter();
    SetNarrow(false);
    Require(Recorder::CommitterRejections() > rejections, what + "the decision off the committer was not rejected");
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Require(ActiveRecorder().Seals() == 0, what + "a decision off the committer left a seal");
}

void NarrowTests(AgcDriver::VulkanDevice& device) {
    {
        GuestBlock audit;
        NarrowAuditCountsOutsideStores(audit);
        NarrowAuditUnionOfRanges(audit);
    }
    NarrowHtileSplitWaits(device);
    NarrowSharedUnitWaits(device);
    NarrowOutsideCompletionSkips(device);
    NarrowAddressReadSkips(device);
    NarrowLaterCompletionsWait(device);
    NarrowCopiedWriterWaits(device);
    NarrowUnitEdges(device);
    NarrowUndeclaredWaits(device);
    NarrowInsideCompletion(device);
    NarrowLabelVisibleAfterImage(device);
    {
        GuestBlock gap;
        NarrowLaterCompletionClosesGap(device, gap);
    }
    NarrowHookableWriteBack(device);
    {
        GuestBlock seal;
        NarrowSealFiresOnOlderCompletion(seal);
    }
    NarrowLabelBehindCompletions(device);
    NarrowTwoLabelsSameSpan(device);
    NarrowThrowingCompletions(device);
    {
        GuestBlock seal;
        GuestBlock labels;
        NarrowSealSurvivesNestedRetirement(seal, labels);
    }
    NarrowRejectsOffCommitter(device);
    NarrowCommitterBindsDuringDecisions(device);
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
        if (std::getenv("APS5_FLUSH_SKIP_SYNC_VERIFY") != nullptr) {
            NarrowAuditVerifyFails();
            std::puts("storage flush skip verify tests passed");
            return 0;
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
        {
            GuestBlock gap;
            LabelReaderLaterCompletionGap(*device, gap);
        }
        NarrowTests(*device);
        std::puts("storage flush skip tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
