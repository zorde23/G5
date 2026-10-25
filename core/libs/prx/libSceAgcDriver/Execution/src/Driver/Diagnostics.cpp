#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace AgcDriver::DriverDetail {

double TraceMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

bool ShaderSkipped(std::uint64_t address) {
    static const std::set<std::uint64_t> skipped = [] {
        std::set<std::uint64_t> addresses;
        const char* text = std::getenv("APS5_SKIP_SHADER");
        while (text != nullptr && *text != '\0') {
            char* end = nullptr;
            const auto value = std::strtoull(text, &end, 16);
            if (end == text) throw std::runtime_error(std::string("APS5_SKIP_SHADER must be hex program addresses separated by commas, got \"") + std::getenv("APS5_SKIP_SHADER") + "\"");
            if (value != 0) addresses.insert(value);
            text = *end == ',' ? end + 1 : end;
        }
        return addresses;
    }();
    return address != 0 && skipped.contains(address);
}

bool SkipRefusedDraws() {
    static const bool skip = std::getenv("APS5_SKIP_REFUSED_DRAWS") != nullptr;
    return skip;
}

void ReportRefusedDraw(const RefusedDraw& refused) {
    static std::mutex reportedMutex;
    static std::set<std::uint64_t> reported;
    {
        std::lock_guard lock(reportedMutex);
        if (!reported.insert(refused.address).second) return;
    }
    const std::string_view what = refused.what();
    const auto line = what.substr(0, what.find('\n'));
    std::fprintf(stderr, "[gpu] APS5_SKIP_REFUSED_DRAWS: skipping the draws that use shader 0x%llx: %.*s\n", static_cast<unsigned long long>(refused.address), static_cast<int>(line.size()), line.data());
}

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    require(it != registers.end(), "required shader register has not been written");
    return it->second;
}

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset) {
    const auto it = shader.find(offset);
    return it == shader.end() ? 0u : it->second;
}

}
