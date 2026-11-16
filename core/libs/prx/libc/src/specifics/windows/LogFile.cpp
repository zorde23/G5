#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <io.h>
#include <string>
#include <windows.h>

namespace {

std::filesystem::path ExeDirectory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool RedirectLog() {
    const char* log = std::getenv("APS5_LOG_FILE");
    if (log == nullptr || *log == '\0' || GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_CHAR) return false;
    const auto directory = ExeDirectory();
    if (directory.empty() || _wfreopen((directory / log).c_str(), L"w", stderr) == nullptr) return false;
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fflush(stdout);
    _dup2(_fileno(stderr), _fileno(stdout));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_ERROR_HANDLE, handle);
    SetStdHandle(STD_OUTPUT_HANDLE, handle);
    return true;
}

const bool logRedirected = RedirectLog();

}
#endif
