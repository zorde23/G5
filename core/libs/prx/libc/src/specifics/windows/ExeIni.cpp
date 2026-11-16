#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

void RedirectToFile(const std::filesystem::path& path) {
    if (_wfreopen(path.c_str(), L"w", stderr) == nullptr) return;
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fflush(stdout);
    _dup2(_fileno(stderr), _fileno(stdout));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_ERROR_HANDLE, handle);
    SetStdHandle(STD_OUTPUT_HANDLE, handle);
}

__attribute__((constructor(101))) void LoadExeIni() {
    const auto directory = ExeDirectory();
    if (directory.empty()) return;
    std::ifstream ini(directory / "g5.ini");
    for (std::string line; std::getline(ini, line);) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        line = Trimmed(line);
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const auto key = Trimmed(line.substr(0, equals));
        if (key.empty() || std::getenv(key.c_str()) != nullptr) continue;
        _putenv_s(key.c_str(), Trimmed(line.substr(equals + 1)).c_str());
    }
    const char* log = std::getenv("APS5_LOG_FILE");
    if (log != nullptr && *log != '\0' && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR) RedirectToFile(directory / log);
}

}
#endif
