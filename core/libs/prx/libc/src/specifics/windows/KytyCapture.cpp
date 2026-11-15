#include "prx/libc/include/KytyCapture.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace {

constexpr std::size_t PageBytes = 0x10000;
constexpr std::uint8_t IntegerRegisters[6] = {7, 6, 2, 1, 8, 9};
constexpr std::uint32_t SaveOffset = 0x20;
constexpr std::uint32_t VectorOffset = SaveOffset + offsetof(KytyCaptureRegisters, xmm);

struct Emitter {
    std::vector<std::uint8_t> code;
    void Bytes(std::initializer_list<std::uint8_t> bytes) { code.insert(code.end(), bytes); }
    void U32(std::uint32_t value) { for (int i = 0; i < 4; ++i) code.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
    void U64(std::uint64_t value) { for (int i = 0; i < 8; ++i) code.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
    void Integer(bool store, std::uint8_t reg, std::uint32_t offset) {
        Bytes({static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0x00)), static_cast<std::uint8_t>(store ? 0x89 : 0x8b), static_cast<std::uint8_t>(0x84 | ((reg & 7) << 3)), 0x24});
        U32(offset);
    }
    void Vector(bool store, std::uint8_t reg, std::uint32_t offset) {
        Bytes({0xf3, 0x0f, static_cast<std::uint8_t>(store ? 0x7f : 0x6f), static_cast<std::uint8_t>(0x84 | (reg << 3)), 0x24});
        U32(offset);
    }
};

std::vector<std::uint8_t> Build(void* target, std::uint64_t index, KytyCaptureRecord record) {
    Emitter e;
    e.Bytes({0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xe4, 0xf0, 0x48, 0x81, 0xec});
    e.U32(0x100);
    for (std::size_t i = 0; i < 6; ++i) e.Integer(true, IntegerRegisters[i], SaveOffset + 8 * static_cast<std::uint32_t>(i));
    e.Integer(true, 0, SaveOffset + offsetof(KytyCaptureRegisters, vectorCount));
    e.Bytes({0x48, 0x8d, 0x45, 0x10});
    e.Integer(true, 0, SaveOffset + offsetof(KytyCaptureRegisters, stack));
    for (std::uint8_t i = 0; i < 8; ++i) e.Vector(true, i, VectorOffset + 16u * i);
    e.Bytes({0x48, 0xb9});
    e.U64(index);
    e.Bytes({0x48, 0x8d, 0x54, 0x24, static_cast<std::uint8_t>(SaveOffset)});
    e.Bytes({0x48, 0xb8});
    e.U64(reinterpret_cast<std::uint64_t>(record));
    e.Bytes({0xff, 0xd0});
    for (std::uint8_t i = 0; i < 8; ++i) e.Vector(false, i, VectorOffset + 16u * i);
    for (std::size_t i = 0; i < 6; ++i) e.Integer(false, IntegerRegisters[i], SaveOffset + 8 * static_cast<std::uint32_t>(i));
    e.Integer(false, 0, SaveOffset + offsetof(KytyCaptureRegisters, vectorCount));
    e.Bytes({0x48, 0x89, 0xec, 0x5d, 0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
    e.U64(reinterpret_cast<std::uint64_t>(target));
    return std::move(e.code);
}

std::mutex g_mutex;
std::uint8_t* g_page = nullptr;
std::size_t g_used = PageBytes;

void* Place(const std::vector<std::uint8_t>& code) {
    std::lock_guard lock(g_mutex);
    if (g_used + code.size() > PageBytes) {
        g_page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, PageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (g_page == nullptr) throw std::runtime_error("Kyty capture: cannot allocate trampoline memory");
        g_used = 0;
    }
    auto* placed = g_page + g_used;
    std::memcpy(placed, code.data(), code.size());
    g_used += (code.size() + 15) & ~std::size_t{15};
    FlushInstructionCache(GetCurrentProcess(), placed, code.size());
    return placed;
}

std::vector<std::uint8_t> BuildReplay() {
    Emitter e;
    const auto fromBlock = [&](std::uint8_t rex, std::uint8_t reg, std::uint32_t offset) {
        e.Bytes({rex, 0x8b, static_cast<std::uint8_t>(0x83 | ((reg & 7) << 3))});
        e.U32(offset);
    };
    e.Bytes({0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x89, 0xfb, 0x49, 0x89, 0xf3, 0x48, 0x83, 0xec, 0x48});
    for (std::uint8_t i = 0; i < 8; ++i) {
        fromBlock(0x48, 0, offsetof(KytyCaptureReplayCall, stackArguments) + 8u * i);
        e.Bytes({0x48, 0x89, 0x44, 0x24, static_cast<std::uint8_t>(8 * i)});
    }
    for (std::uint8_t i = 0; i < 8; ++i) {
        e.Bytes({0xf3, 0x0f, 0x6f, static_cast<std::uint8_t>(0x83 | (i << 3))});
        e.U32(static_cast<std::uint32_t>(offsetof(KytyCaptureRegisters, xmm) + 16u * i));
    }
    fromBlock(0x48, 6, offsetof(KytyCaptureRegisters, integers) + 8);
    fromBlock(0x48, 2, offsetof(KytyCaptureRegisters, integers) + 16);
    fromBlock(0x48, 1, offsetof(KytyCaptureRegisters, integers) + 24);
    fromBlock(0x4c, 8, offsetof(KytyCaptureRegisters, integers) + 32);
    fromBlock(0x4c, 9, offsetof(KytyCaptureRegisters, integers) + 40);
    fromBlock(0x48, 0, offsetof(KytyCaptureRegisters, vectorCount));
    fromBlock(0x48, 7, offsetof(KytyCaptureRegisters, integers));
    e.Bytes({0x41, 0xff, 0xd3, 0x48, 0x83, 0xc4, 0x48, 0x5b, 0x5d, 0xc3});
    return std::move(e.code);
}

}

std::uint64_t KytyCaptureReplay(const KytyCaptureReplayCall& call, void* target) {
    using Thunk = std::uint64_t(APS5_VABI*)(const KytyCaptureReplayCall*, void*);
    static const auto thunk = reinterpret_cast<Thunk>(Place(BuildReplay()));
    return thunk(&call, target);
}

void* KytyCaptureTrampoline(void* target, std::uint64_t index, KytyCaptureRecord record) {
    return Place(Build(target, index, record));
}

#else

void* KytyCaptureTrampoline(void* target, std::uint64_t, KytyCaptureRecord) {
    return target;
}

std::uint64_t KytyCaptureReplay(const KytyCaptureReplayCall&, void*) {
    throw std::runtime_error("Kyty capture replay is Windows only");
}

#endif
