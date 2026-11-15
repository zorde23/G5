#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_KYTYGPU_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_KYTYGPU_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// The Kyty GPU hybrid's import binding (APS5_KYTY_GPU; known gaps in docs/dev/debt/libc.md).
void* KytyGpuImport(void* target, const char* name, const char* library);
bool KytyGpuEnabled();
void KytyGpuOnMap(std::uintptr_t address, std::size_t bytes, std::uint32_t protection, void* section, std::uint64_t offset);
void KytyGpuOnUnmap(std::uintptr_t address, std::size_t bytes);
void KytyGpuOnProtect(std::uintptr_t address, std::size_t bytes, std::uint32_t protection);
void KytyGpuOnSectionClosed(void* section);
void KytyGpuNoteGameProtection(std::uintptr_t address, std::size_t bytes, std::uint32_t protection);
void KytyGpuReapplyProtection(std::uintptr_t address, std::size_t bytes);
bool KytyGpuProtectHost(std::uint64_t address, std::uint64_t bytes, std::uint32_t tracker);
void KytyGpuPrepareHostWrite(std::uintptr_t address, std::size_t bytes);
bool KytyGpuHandleFault(std::uintptr_t address, std::uint32_t access, std::uint64_t rip, void* context);
void KytyGpuReportCounters();
extern "C" {
void KytyGpuCaptureTrack_nid_no_patch(std::uintptr_t address, std::size_t bytes, bool on);
void KytyGpuCaptureTakeWritten_nid_no_patch(std::uintptr_t address, std::size_t bytes, std::vector<std::uintptr_t>* written);
std::uint64_t KytyGpuCaptureFaults_nid_no_patch();
bool KytyGpuReadableWithoutFault_nid_no_patch(std::uintptr_t address, std::size_t bytes);
}

#endif
