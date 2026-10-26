#include "prx/libkernel/include/Api.hpp"
#include <windows.h>
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t Page = 0x4000;
constexpr DWORD Terminated = 0xC0000409u;

[[gnu::noinline]] void Store(volatile char* target) {
    *target = 1;
}

[[gnu::noinline]] int Elsewhere(int value) {
    return value * 3 + 1;
}

std::uintptr_t Rva(const void* function) {
    return reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

int Child() {
    std::int64_t physical = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, Page, 0, 0, &physical) == 0, "allocate direct memory");
    void* mapping = nullptr;
    void* twin = nullptr;
    Require(sceKernelMapDirectMemory(&mapping, Page, 3, 0, physical, 0) == 0, "map direct memory");
    Require(sceKernelMapDirectMemory(&twin, Page, 3, 0, physical, 0) == 0, "map direct memory a second time");
    std::array<void*, 4> pages{};
    std::size_t count = pages.size();
    Require(GuestArena::GuestArenaCollectWrites_nid_postfix(reinterpret_cast<std::uintptr_t>(mapping), Page, pages.data(), &count, true), "collect tracked writes");
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(mapping, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_READONLY, "the shared page is armed");
    Store(static_cast<volatile char*>(mapping));
    Require(static_cast<volatile char*>(twin)[0] == 1, "the resumed store reached the page");
    return Elsewhere(0) == 1 ? 0 : 2;
}

DWORD RunChild(const wchar_t* self, const std::uintptr_t begin) {
    char tablePath[MAX_PATH * 3];
    std::snprintf(tablePath, sizeof(tablePath), "%ls.redzone.txt", self);
    {
        std::ofstream table(tablePath, std::ios::trunc);
        char line[64];
        std::snprintf(line, sizeof(line), "%llx %llx test leaf\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(begin + 0x40));
        table << line;
    }
    std::wstring command = L"\"" + std::wstring(self) + L"\" child";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != 0, "start the child");
    Require(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0, "the child finished");
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::remove(tablePath);
    return code;
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::strcmp(argv[1], "child") == 0) return Child();
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        SetEnvironmentVariableW(L"APS5_RED_ZONE_GUARD", L"1");
        Require(RunChild(self, Rva(reinterpret_cast<const void*>(&Elsewhere))) == 0, "a resumed fault outside every listed range was not resumed");
        Require(RunChild(self, Rva(reinterpret_cast<const void*>(&Store))) == Terminated, "a resumed fault inside a listed red-zone function did not terminate the process");
        std::puts("red-zone guard tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "red-zone guard tests failed: %s\n", error.what());
        return 1;
    }
}
