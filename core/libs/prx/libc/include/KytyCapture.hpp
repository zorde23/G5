#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_KYTYCAPTURE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_KYTYCAPTURE_HPP

#include <cstdint>

struct KytyCaptureRegisters {
    std::uint64_t integers[6];
    std::uint64_t vectorCount;
    std::uint64_t stack;
    std::uint64_t xmm[8][2];
};

using KytyCaptureRecord = void (*)(std::uint64_t index, const KytyCaptureRegisters* registers);

void* KytyCaptureTrampoline(void* target, std::uint64_t index, KytyCaptureRecord record);

#endif
