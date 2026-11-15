#include "prx/libc/include/KytyCapture.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

KytyCaptureRegisters g_seen{};
std::uint64_t g_index = 0;
int g_calls = 0;

void Require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

void Record(std::uint64_t index, const KytyCaptureRegisters* registers) {
    g_index = index;
    g_seen = *registers;
    ++g_calls;
    double scratch[8];
    for (int i = 0; i < 8; ++i) scratch[i] = static_cast<double>(i) * -1.5;
    asm volatile("" : : "r"(scratch) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11");
}

using Target = std::uint64_t (__attribute__((sysv_abi)) *)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, double, double, double, double, double, double, double, double, std::uint64_t, std::uint64_t);

[[gnu::noinline]] std::uint64_t __attribute__((sysv_abi)) Sum(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f, double x0, double x1, double x2, double x3, double x4, double x5, double x6, double x7, std::uint64_t s0, std::uint64_t s1) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + static_cast<std::uint64_t>(x0 + 2 * x1 + 3 * x2 + 4 * x3 + 5 * x4 + 6 * x5 + 7 * x6 + 8 * x7) + 100 * s0 + 1000 * s1;
}

double XmmLow(int index) {
    double value = 0;
    std::memcpy(&value, &g_seen.xmm[index][0], sizeof(value));
    return value;
}

}

int main() {
    try {
        auto* trampoline = reinterpret_cast<Target>(KytyCaptureTrampoline(reinterpret_cast<void*>(&Sum), 42, &Record));
        const std::uint64_t expected = Sum(1, 2, 3, 4, 5, 6, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 7, 9);
        const std::uint64_t returned = trampoline(1, 2, 3, 4, 5, 6, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 7, 9);
        Require(g_calls == 1 && g_index == 42, "the trampoline did not call the recorder once with its index");
        Require(returned == expected, "the call through the trampoline did not return the target's value");
        for (int i = 0; i < 6; ++i) Require(g_seen.integers[i] == static_cast<std::uint64_t>(i + 1), "an integer argument register was not recorded");
        for (int i = 0; i < 8; ++i) Require(XmmLow(i) == 0.5 + i, "a vector argument register was not recorded");
        const auto* stack = reinterpret_cast<const std::uint64_t*>(g_seen.stack);
        Require(stack[0] == 7 && stack[1] == 9, "the stack arguments are not where the recorded stack pointer says");
        Require((g_seen.stack & 7) == 0, "the recorded stack pointer is not the caller's argument area");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kyty capture trampoline: %s\n", error.what());
        return 1;
    }
    std::puts("kyty capture trampoline: ok");
    return 0;
}
