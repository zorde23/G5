#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_KYTYGPU_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_KYTYGPU_HPP

#include <cstddef>
#include <cstdint>

// The Kyty GPU hybrid's import binding (APS5_KYTY_GPU; known gaps in docs/dev/debt/libc.md).
void* KytyGpuImport(void* target, const char* name, const char* library);
bool KytyGpuEnabled();
void KytyGpuOnMap(std::uintptr_t address, std::size_t bytes, std::uint32_t protection, void* section, std::uint64_t offset);
void KytyGpuOnUnmap(std::uintptr_t address, std::size_t bytes);
void KytyGpuOnProtect(std::uintptr_t address, std::size_t bytes, std::uint32_t protection);
void KytyGpuOnSectionClosed(void* section);

#endif
