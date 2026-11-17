#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <io.h>
#include <set>
#include <string>
#include <utility>
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

std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

void LogInit() {
    std::string fromIni;
    std::string overridden;
    const auto directory = ExeDirectory();
    std::set<std::wstring> seen;
    for (const auto& [file, name] : {std::pair{L"ps5-hybrid.ini", L"hybrid"}, std::pair{L"g5.ini", L"g5"}}) {
        std::wstring section(8192, L'\0');
        const auto length = directory.empty() ? 0 : GetPrivateProfileSectionW(name, section.data(), static_cast<DWORD>(section.size()), (directory / file).c_str());
        for (std::size_t start = 0; start < length;) {
            const std::wstring entry(section.c_str() + start);
            start += entry.size() + 1;
            const auto equals = entry.find(L'=');
            if (equals == std::wstring::npos) continue;
            const auto key = entry.substr(0, equals);
            if (!seen.insert(key).second) continue;
            const wchar_t* value = _wgetenv(key.c_str());
            auto& list = value != nullptr && entry.compare(equals + 1, std::wstring::npos, value) == 0 ? fromIni : overridden;
            list += (list.empty() ? "" : " ") + std::filesystem::path(key).string();
        }
    }
    const auto language = Env("ANYPS5_LANGUAGE");
    std::fprintf(stderr, "[hybrid] init: guest MXCSR %s, mimalloc %s, language %s; keys from ps5-hybrid.ini or g5.ini (or the same value already in the environment): %s; ini keys the environment overrides: %s\n",
        Env("APS5_GUEST_MXCSR") == "host" ? "host (diagnostic)" : "0x9fc0 (PS5)", GetModuleHandleW(L"mimalloc.dll") != nullptr ? "loaded" : "off",
        language.empty() ? "default" : language.c_str(), fromIni.empty() ? "none" : fromIni.c_str(), overridden.empty() ? "none" : overridden.c_str());
}

const bool logRedirected = RedirectLog();
const bool initLogged = (LogInit(), true);

}
#endif
