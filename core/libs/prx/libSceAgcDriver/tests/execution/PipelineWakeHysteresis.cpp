#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawFinisher;
using AgcDriver::DriverDetail::DrawPipeline;

void Spin(std::chrono::microseconds length) {
    const auto until = std::chrono::steady_clock::now() + length;
    while (std::chrono::steady_clock::now() < until) {
    }
}

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

}

int main() {
    try {
        if (DrawPipeline::Depth() == 0 || !DrawFinisher::Enabled()) return 77;
        constexpr std::uint32_t Jobs = 20000;
        constexpr std::uint32_t Phase = 2500;
        std::vector<std::uint32_t> committed;
        committed.reserve(Jobs + Jobs / 64);
        std::atomic<std::uint32_t> finished{0};
        std::uint32_t nonDraw = 0;
        for (std::uint32_t job = 0; job < Jobs; ++job) {
            const bool committerSlow = (job / Phase) % 2 == 0;
            DrawFinisher::Get().Enqueue([job, committerSlow, &committed, &finished] {
                if (!committerSlow) Spin(std::chrono::microseconds(20));
                finished.fetch_add(1, std::memory_order_relaxed);
                DrawPipeline::Queue0().Enqueue([job, committerSlow, &committed] {
                    if (committerSlow) Spin(std::chrono::microseconds(20));
                    committed.push_back(job);
                }, {}, 0, {}, {}, true);
            });
            if (job % 64 == 63) {
                const auto marker = Jobs + nonDraw++;
                DrawPipeline::Queue0().Enqueue([marker, &committed] { committed.push_back(marker); }, {});
            }
            if (job % 1000 == 999) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        }
        DrawFinisher::Get().Drain();
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        Require(finished.load() == Jobs, "the draw finisher ran " + std::to_string(finished.load()) + " of " + std::to_string(Jobs) + " jobs");
        Require(committed.size() == Jobs + nonDraw, "the committer ran " + std::to_string(committed.size()) + " of " + std::to_string(Jobs + nonDraw) + " commits");
        std::uint32_t next = 0;
        std::uint32_t nextMarker = Jobs;
        for (const auto value : committed) {
            if (value < Jobs) {
                Require(value == next, "draw commit " + std::to_string(value) + " ran out of order (expected " + std::to_string(next) + ")");
                ++next;
            } else {
                Require(value == nextMarker, "non-draw commit " + std::to_string(value) + " ran out of order");
                ++nextMarker;
            }
        }
        constexpr std::uint32_t PerProducer = 5000;
        std::vector<std::uint32_t> producerCommits[2];
        producerCommits[0].reserve(PerProducer);
        producerCommits[1].reserve(PerProducer);
        std::atomic<bool> producing{true};
        const auto produce = [&](std::uint32_t producer) {
            for (std::uint32_t item = 0; item < PerProducer; ++item) {
                DrawPipeline::Queue0().Enqueue([producer, item, &producerCommits] {
                    Spin(std::chrono::microseconds(10));
                    producerCommits[producer].push_back(item);
                }, {}, 0, {}, {}, true);
            }
        };
        std::thread first(produce, 0u);
        std::thread second(produce, 1u);
        std::thread drainer([&] {
            while (producing.load()) {
                DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });
        first.join();
        second.join();
        producing = false;
        drainer.join();
        DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
        for (std::uint32_t producer = 0; producer < 2; ++producer) {
            Require(producerCommits[producer].size() == PerProducer, "producer " + std::to_string(producer) + " committed " + std::to_string(producerCommits[producer].size()) + " of " + std::to_string(PerProducer));
            for (std::uint32_t item = 0; item < PerProducer; ++item) Require(producerCommits[producer][item] == item, "producer " + std::to_string(producer) + " commit " + std::to_string(item) + " ran out of order");
        }
        Require(DrawPipeline::WakeSafetyTimeouts() == 0, "a pipeline wait found its condition already met at the safety timeout " + std::to_string(DrawPipeline::WakeSafetyTimeouts()) + " times");
        std::cout << "pipeline wake: " << Jobs << " finisher jobs and " << nonDraw << " non-draw commits in order, two producers of " << PerProducer << " beside a drainer in order, hysteresis " << (DrawPipeline::WakeHysteresis() ? "on" : "off") << ", safety timeouts " << DrawPipeline::WakeSafetyTimeouts() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
