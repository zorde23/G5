#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Step {
    std::uint32_t button = 0;
    std::int64_t ms = 0;
};

struct Phase {
    std::vector<Step> steps;
    std::int64_t intervalMs = 0;
    std::int64_t durationMs = 0;
    std::int64_t startMs = 0;
    bool menuOnly = false;
};

struct Plan {
    std::vector<Phase> phases;
    Clock::time_point start;
    bool started = false;
};

std::uint32_t ButtonNamed(const std::string& name) {
    static const std::pair<const char*, std::uint32_t> names[] = {{"cross", 0x4000}, {"circle", 0x2000}, {"square", 0x8000}, {"triangle", 0x1000}, {"r1", 0x0800}, {"l1", 0x0400}, {"r2", 0x0200}, {"l2", 0x0100}, {"r3", 0x0004}, {"l3", 0x0002}, {"up", 0x0010}, {"down", 0x0040}, {"left", 0x0080}, {"right", 0x0020}, {"options", 0x0008}, {"touchpad", 0x100000}};
    for (const auto& [text, button] : names) {
        if (name == text) return button;
    }
    return 0;
}

Phase ParsePhase(const std::string& value) {
    Phase parsed;
    const auto first = value.find(';');
    const auto second = first == std::string::npos ? std::string::npos : value.find(';', first + 1);
    if (second == std::string::npos) return parsed;
    const auto third = value.find(';', second + 1);
    const auto list = value.substr(0, first);
    parsed.intervalMs = std::strtoll(value.c_str() + first + 1, nullptr, 10);
    parsed.durationMs = std::strtoll(value.c_str() + second + 1, nullptr, 10) * 1000;
    if (third != std::string::npos) parsed.startMs = std::strtoll(value.c_str() + third + 1, nullptr, 10) * 1000;
    if (const auto fourth = third == std::string::npos ? std::string::npos : value.find(';', third + 1); fourth != std::string::npos) parsed.menuOnly = value.compare(fourth + 1, 1, "m") == 0;
    for (std::size_t at = 0; at < list.size();) {
        auto end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        const auto item = list.substr(at, end - at);
        if (const auto colon = item.find(':'); colon != std::string::npos) {
            if (const auto button = ButtonNamed(item.substr(0, colon)); button != 0) parsed.steps.push_back({button, std::strtoll(item.c_str() + colon + 1, nullptr, 10)});
        }
        at = end + 1;
    }
    if (parsed.steps.empty() || parsed.intervalMs <= 0) parsed.steps.clear();
    else std::fprintf(stderr, "[tmp-autopress] %zu steps, every %lld ms, for %lld s from %lld s\n", parsed.steps.size(), static_cast<long long>(parsed.intervalMs), static_cast<long long>(parsed.durationMs / 1000), static_cast<long long>(parsed.startMs / 1000));
    return parsed;
}

Plan& Autopress() {
    static Plan plan = [] {
        Plan parsed;
        const char* text = std::getenv("APS5_TMP_AUTOPRESS");
        if (text == nullptr) return parsed;
        const std::string value(text);
        for (std::size_t at = 0; at <= value.size();) {
            auto end = value.find('|', at);
            if (end == std::string::npos) end = value.size();
            if (auto phase = ParsePhase(value.substr(at, end - at)); !phase.steps.empty()) parsed.phases.push_back(std::move(phase));
            at = end + 1;
        }
        return parsed;
    }();
    return plan;
}

bool InWorld(Clock::time_point now) {
    static bool latched = false;
    static Clock::time_point lastCheck{};
    static const char* stop = std::getenv("APS5_TMP_AUTOPRESS_STOP");
    if (latched) return true;
    if (stop == nullptr || now - lastCheck < std::chrono::seconds(1)) return false;
    lastCheck = now;
    if (std::FILE* file = std::fopen(stop, "rb")) {
        std::fclose(file);
        latched = true;
        std::fprintf(stderr, "[tmp-autopress] world detected by the bench (draws): menu presses stop for good\n");
    }
    return latched;
}

std::uint32_t IdleTap(Clock::time_point now) {
    static const std::uint32_t button = [] {
        const char* text = std::getenv("APS5_TMP_AUTOPRESS_IDLE_BUTTON");
        return text != nullptr ? ButtonNamed(text) : 0u;
    }();
    static const std::int64_t periodMs = [] {
        const char* text = std::getenv("APS5_TMP_AUTOPRESS_IDLE_SEC");
        const auto seconds = text != nullptr ? std::strtoll(text, nullptr, 10) : 0ll;
        return (seconds > 0 ? seconds : 20ll) * 1000;
    }();
    static Clock::time_point next{}, until{};
    static bool armed = false;
    if (button == 0 || !InWorld(now)) return 0;
    if (!armed) {
        armed = true;
        next = now + std::chrono::milliseconds(periodMs);
        std::fprintf(stderr, "[tmp-autopress] idle taps every %lld s from now\n", static_cast<long long>(periodMs / 1000));
    }
    if (now >= next) {
        next = now + std::chrono::milliseconds(periodMs);
        until = now + std::chrono::milliseconds(100);
    }
    return now < until ? button : 0u;
}

}

std::uint32_t KytyAutopressButtons() {
    auto& plan = Autopress();
    if (plan.phases.empty()) return 0;
    const auto now = Clock::now();
    if (const auto tap = IdleTap(now); tap != 0) return tap;
    if (!plan.started) {
        plan.started = true;
        plan.start = now;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - plan.start).count();
    for (const auto& phase : plan.phases) {
        if (elapsed < phase.startMs || elapsed >= phase.startMs + phase.durationMs) continue;
        if (phase.menuOnly && InWorld(now)) continue;
        auto offset = (elapsed - phase.startMs) % phase.intervalMs;
        for (const auto& step : phase.steps) {
            if (offset < step.ms) return step.button;
            offset -= step.ms + 200;
            if (offset < 0) break;
        }
    }
    return 0;
}
