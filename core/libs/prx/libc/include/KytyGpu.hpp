#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_KYTYGPU_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_KYTYGPU_HPP

// The Kyty GPU hybrid's import binding (APS5_KYTY_GPU; known gaps in docs/dev/debt/libc.md).
void* KytyGpuImport(void* target, const char* name, const char* library);
bool KytyGpuEnabled();

#endif
