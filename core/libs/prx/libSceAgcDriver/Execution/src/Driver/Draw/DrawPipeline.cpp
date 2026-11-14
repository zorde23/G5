#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission"};

std::atomic<unsigned> runningCommitters{0};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

std::atomic<std::uint64_t> heldReadRanges{0};
std::atomic<std::uint64_t> heldReadQueries{0};
std::atomic<std::uint64_t> heldReadHits{0};

}

namespace {
thread_local bool finisherThread = false;
thread_local bool finisherProducer = false;
}

DrawFinisher& DrawFinisher::Get() {
    static DrawFinisher* finisher = new DrawFinisher();
    return *finisher;
}

bool DrawFinisher::Enabled() {
    return Graphics::DrawPipelineEnabled();
}

bool DrawPipeline::MetadataPassesPipelined() {
    return Graphics::DrawPipelineEnabled();
}

namespace {
std::atomic<std::uint64_t> pipelinedMetadataPasses{0};
}

std::uint64_t DrawPipeline::PipelinedMetadataPasses() {
    return pipelinedMetadataPasses.load(std::memory_order_relaxed);
}

void DrawPipeline::CountPipelinedMetadataPass() {
    pipelinedMetadataPasses.fetch_add(1, std::memory_order_relaxed);
}

bool DrawFinisher::Overlap() {
    return Graphics::DrawPipelineEnabled();
}

bool DrawFinisher::PendingOverlaps(std::uint64_t begin, std::uint64_t end) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    for (const auto& held : pending) {
        if (held.unknown) return true;
        for (const auto& [first, last] : held.writes) {
            if (begin < last && first < end) return true;
        }
    }
    return false;
}

DrawFinisher::LabelLookup DrawFinisher::PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value) {
    if (!Busy() || !finisherProducer) return LabelLookup::NotHeld;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto held = pending.rbegin(); held != pending.rend(); ++held) {
        if (held->unknown) return LabelLookup::Unknown;
        const bool overlaps = std::any_of(held->writes.begin(), held->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (held->labelBytes.empty() || address < held->labelAddress || end > held->labelAddress + held->labelBytes.size()) return LabelLookup::Unknown;
        value = 0;
        std::memcpy(&value, held->labelBytes.data() + (address - held->labelAddress), bytes);
        return LabelLookup::Value;
    }
    return LabelLookup::NotHeld;
}

bool DrawFinisher::PendingOverlapsAny(std::span<const Range> ranges) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    for (const auto& held : pending) {
        if (held.unknown) return true;
        for (const auto& [first, last] : held.writes) {
            for (const auto& [begin, end] : ranges) {
                if (begin < last && first < end) return true;
            }
        }
    }
    return false;
}

bool DrawFinisher::PendingKey(std::uint64_t key, std::uint64_t shape) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    return std::any_of(pending.begin(), pending.end(), [&](const Pending& held) { return held.key == key || (shape != 0 && held.shape == shape); });
}

namespace {
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawFinisher::DrainSite::Count)> finisherDrains{}, finisherSkips{};
}

void DrawFinisher::NoteDrain(DrainSite site, bool drained) {
    (drained ? finisherDrains : finisherSkips)[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
}

namespace {
std::array<std::atomic<std::uint64_t>, 257> drainedOpcodes{}, drainedBeside{};
}

void DrawFinisher::NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits) {
    drainedOpcodes[std::min<std::uint32_t>(opcode, 256u)].fetch_add(1, std::memory_order_relaxed);
    if (besideCommits) drainedBeside[std::min<std::uint32_t>(opcode, 256u)].fetch_add(1, std::memory_order_relaxed);
}

std::string DrawFinisher::DrainText() {
    constexpr std::array<const char*, static_cast<std::size_t>(DrainSite::Count)> names{"lookup", "reads", "packet", "kept draw"};
    std::string text = "; worker drains of the finisher (drained/skipped):";
    for (std::size_t i = 0; i < names.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu/%llu", names[i], static_cast<unsigned long long>(finisherDrains[i].exchange(0)), static_cast<unsigned long long>(finisherSkips[i].exchange(0)));
        text += item;
    }
    text += "; drained by opcode (of them beside commits):";
    for (int shown = 0; shown < 6; ++shown) {
        std::size_t top = 0;
        for (std::size_t i = 1; i < drainedOpcodes.size(); ++i) {
            if (drainedOpcodes[i].load() > drainedOpcodes[top].load()) top = i;
        }
        const auto count = drainedOpcodes[top].exchange(0);
        if (count == 0) break;
        char item[48];
        std::snprintf(item, sizeof(item), " 0x%02zx x%llu (%llu)", top, static_cast<unsigned long long>(count), static_cast<unsigned long long>(drainedBeside[top].exchange(0)));
        text += item;
    }
    return text;
}

bool DrawFinisher::OnFinisherThread() {
    return finisherThread;
}

void DrawFinisher::Enqueue(Job job) {
    Enqueue(std::move(job), Pending{});
}

namespace {

constexpr std::size_t FinisherDepth = 16;
std::atomic<std::uint64_t> wakeSafetyTimeouts{0};

template <class Ready>
void waitCounted(std::condition_variable& condition, std::unique_lock<std::mutex>& lock, const std::uint64_t& notifies, Ready ready) {
    if (!DrawPipeline::WakeHysteresis()) {
        condition.wait(lock, ready);
        return;
    }
    while (!ready()) {
        const auto before = notifies;
        if (condition.wait_for(lock, std::chrono::milliseconds(250)) != std::cv_status::timeout || !ready() || notifies != before) continue;
        const auto count = wakeSafetyTimeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        std::fprintf(stderr, "[draw] wake hysteresis: a pipeline wait found its condition already met at its 250 ms safety timeout, a possible missed wake (%llu so far)\n", static_cast<unsigned long long>(count));
    }
}

}

bool DrawPipeline::WakeHysteresis() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_PIPELINE_WAKE_HYSTERESIS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

std::uint64_t DrawPipeline::WakeSafetyTimeouts() {
    return wakeSafetyTimeouts.load(std::memory_order_relaxed);
}

void DrawFinisher::Enqueue(Job job, Pending held) {
    finisherProducer = true;
    constexpr std::size_t depth = FinisherDepth;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Job>, ReclaimStorage>();
    reclaim.clear();
    std::unique_lock lock(mutex);
    if (!ran.empty()) std::swap(ran, reclaim);
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
    if (jobs.size() >= depth && GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher full while the enqueuing thread holds the GPU lock");
    if (jobs.size() >= depth) {
        const std::size_t resume = DrawPipeline::WakeHysteresis() ? depth / 2 : depth - 1;
        ++fullWaiters;
        struct Waiting {
            std::uint32_t& count;
            ~Waiting() { --count; }
        } waiting{fullWaiters};
        waitCounted(idle, lock, idleNotifies, [&] { return jobs.size() <= resume; });
    }
    jobs.push_back(std::move(job));
    pending.push_back(std::move(held));
    outstanding.fetch_add(1, std::memory_order_release);
    if (!running) {
        running = true;
        thread = std::thread([this] {
            finisherThread = true;
            GuestMemory::MarkFinisherProofThread();
            PinFinisherThread();
            run();
        });
    }
    if (sleeping) wake.notify_one();
}

bool ThreadPhasesProfiled() {
    static const bool enabled = std::getenv("APS5_THREAD_PHASES") != nullptr;
    return enabled;
}

void NoteThreadPhases(bool finisher, std::uint64_t waitNs, std::uint64_t workNs, std::uint64_t afterNs) {
    struct Phases {
        std::atomic<std::uint64_t> wait{0}, work{0}, after{0}, items{0};
        std::atomic<std::int64_t> reportedAt{0}, lastNs{0};
    };
    static std::array<Phases, 2> phases;
    auto& own = phases[finisher ? 0 : 1];
    own.wait.fetch_add(waitNs, std::memory_order_relaxed);
    own.work.fetch_add(workNs, std::memory_order_relaxed);
    own.after.fetch_add(afterNs, std::memory_order_relaxed);
    own.items.fetch_add(1, std::memory_order_relaxed);
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto tenSeconds = nowNs / 10'000'000'000ll;
    if (own.reportedAt.exchange(tenSeconds, std::memory_order_relaxed) == tenSeconds) return;
    const auto previous = own.lastNs.exchange(nowNs, std::memory_order_relaxed);
    const auto ms = [](std::atomic<std::uint64_t>& value) { return static_cast<double>(value.exchange(0, std::memory_order_relaxed)) / 1e6; };
    const double wait = ms(own.wait), work = ms(own.work), after = ms(own.after);
    const auto items = own.items.exchange(0, std::memory_order_relaxed);
    if (previous == 0) return;
    const double wall = static_cast<double>(nowNs - previous) / 1e6;
    AgcDriver::ProfilePrint_nid_no_patch(finisher ? "[f-phases] 10 s (draw finisher threads, summed), ms: wall %.1f per thread; waiting for a job %.1f, job bodies %.1f (%llu jobs), bookkeeping after a job %.1f\n" : "[w-phases] 10 s (queue 0 worker), ms: wall %.1f; waiting for a submission %.1f, executing %.1f (%llu submissions), completion and the rest after it %.1f\n", wall, wait, work, static_cast<unsigned long long>(items), after);
}

void DrawFinisher::run() {
    PerformanceTrace::BindThread("draw finisher");
    const bool phased = ThreadPhasesProfiled();
    auto loopStart = phased ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (;;) {
        Job* job = nullptr;
        bool failed = false;
        if (outstanding.load(std::memory_order_acquire) == 0) ShaderRecompiler::ReleaseDeferredResults();
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        {
            std::unique_lock lock(mutex);
            sleeping = true;
            wake.wait(lock, [&] { return !jobs.empty(); });
            sleeping = false;
            job = &jobs.front();
            failed = failure != nullptr;
        }
        const auto picked = phased ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::exception_ptr error;
        try {
            if (!failed) (*job)();
        } catch (...) {
            error = std::current_exception();
        }
        const auto worked = phased ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        struct PhaseNote {
            bool phased;
            std::chrono::steady_clock::time_point& loopStart;
            std::chrono::steady_clock::time_point picked, worked;
            ~PhaseNote() {
                if (!phased) return;
                const auto now = std::chrono::steady_clock::now();
                const auto ns = [](auto from, auto to) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count()); };
                NoteThreadPhases(true, ns(loopStart, picked), ns(picked, worked), ns(worked, now));
                loopStart = now;
            }
        } phaseNote{phased, loopStart, picked, worked};
        std::lock_guard lock(mutex);
        if (error != nullptr && failure == nullptr) failure = error;
        ran.push_back(std::move(jobs.front()));
        jobs.pop_front();
        pending.pop_front();
        outstanding.fetch_sub(1, std::memory_order_release);
        if (!DrawPipeline::WakeHysteresis() || jobs.empty() || failure != nullptr || (fullWaiters != 0 && jobs.size() <= FinisherDepth / 2)) {
            ++idleNotifies;
            idle.notify_all();
        }
    }
}

void DrawFinisher::Settle() {
    if (!Busy() || OnFinisherThread()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher settle under the GPU lock");
    std::unique_lock lock(mutex);
    waitCounted(idle, lock, idleNotifies, [&] { return jobs.empty(); });
}

void DrawFinisher::Drain() {
    if (OnFinisherThread()) return;
    if (Busy() && GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher drain under the GPU lock");
    SpinUntil(HandoffSpinWindow() * 2, [&] { return outstanding.load(std::memory_order_acquire) == 0; });
    std::unique_lock lock(mutex);
    if (!jobs.empty()) waitCounted(idle, lock, idleNotifies, [&] { return jobs.empty(); });
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
}

void DrawFinisher::ReleaseRan() {
    std::vector<Job> done;
    std::lock_guard lock(mutex);
    std::swap(done, ran);
}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{8};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{8} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

std::size_t DrawPipeline::QueuedItems() {
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    return pipeline.items.size();
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch();
}

unsigned DrawPipeline::Committers() {
    return runningCommitters.load(std::memory_order_acquire);
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads, bool draw) {
    if (DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread()) DrawFinisher::Get().Drain();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Item>, ReclaimStorage>();
    reclaim.clear();
    const auto lockStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::unique_lock lock(mutex);
    if (!retired.empty()) std::swap(retired, reclaim);
    if (profile) lockWaitNs += elapsedNs(lockStart);
    rethrowFailure();
    if (closing) {
        lock.unlock();
        commit();
        return;
    }
    if (items.size() >= Depth()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline full while the enqueuing thread holds the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        if (WakeHysteresis()) {
            const auto resume = Depth() / 2;
            ++fullWaiters;
            struct Waiting {
                std::uint32_t& count;
                ~Waiting() { --count; }
            } waiting{fullWaiters};
            waitCounted(idle, lock, idleNotifies, [&] { return items.size() <= resume; });
        } else {
            resumeAt = Depth() - 1;
            ++idleWaiters;
            idle.wait(lock, [&] { return items.size() <= resumeAt; });
            --idleWaiters;
            resumeAt = 0;
        }
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    heldReadRanges.fetch_add(reads.size(), std::memory_order_relaxed);
    countWrites(writes, true);
    items.push_back({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads), draw, GuestMemory::CurrentPacket().opcode});
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) {
        thread = std::thread([this] {
            GuestMemory::MarkProofThread();
            run();
        });
        runningCommitters.fetch_add(1, std::memory_order_acq_rel);
    }
    if (committerWaiting) {
        ++wakes;
        wake.notify_one();
    }
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr;
    if (!profile && !stats) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    const auto decodeDrainCount = decodeDrains.load(std::memory_order_relaxed);
    char reads[96];
    std::snprintf(reads, sizeof(reads), " %llu read ranges, %llu stores held, %llu decode drains", static_cast<unsigned long long>(heldReadRanges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(heldReadHits.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(decodeDrainCount - reportedDecodeDrains));
    reportedDecodeDrains = decodeDrainCount;
    drainText += reads;
    AgcDriver::ProfilePrint_nid_no_patch("[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room, %.2f us for the lock; %llu committer wakes;%s; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, enqueued == 0 ? 0.0 : static_cast<double>(lockWaitNs) / 1e3 / static_cast<double>(enqueued), static_cast<unsigned long long>(wakes), reads, drainText.c_str());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = wakes = lockWaitNs = 0;
    drains = {};
    drainWaitNs = {};
}


void DrawPipeline::run() {
    Graphics::Recorder::BindCommitter();
    struct Unbind {
        ~Unbind() { Graphics::Recorder::UnbindCommitter(); }
    } unbind;
    PerformanceTrace::BindThread("draw committer");
    const bool reservedCore = PinCommitterThread();
    StartCommitterSampler();
    for (;;) {
        Commit* commit = nullptr;
        bool isDraw = false;
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            committerWaiting = false;
            if (items.empty()) break;
            commit = stopped ? nullptr : &items.front().commit;
            isDraw = items.front().draw;
        }
        const auto start = std::chrono::steady_clock::now();
        if (reservedCore) NoteCommitterCore(start);
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
        try {
            if (commit != nullptr) (*commit)();
            if (!isDraw) GuestMemory::BreakProofs();
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        const auto spent = elapsedNs(start);
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        std::lock_guard lock(mutex);
        countWrites(items.front().writes, false);
        retired.push_back(std::move(items.front()));
        items.pop_front();
        retiredItems.fetch_add(1, std::memory_order_release);
        ++commits;
        commitNs += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        if (retireWaiters != 0 || (idleWaiters != 0 && (items.empty() || items.size() <= resumeAt)) || (fullWaiters != 0 && (items.size() <= Depth() / 2 || failure != nullptr))) {
            ++idleNotifies;
            idle.notify_all();
        }
    }
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

void DrawPipeline::Stop() {
    std::thread committer;
    {
        std::lock_guard lock(mutex);
        closing = true;
        committer = std::move(thread);
    }
    wake.notify_all();
    if (!committer.joinable()) return;
    if (committer.get_id() == std::this_thread::get_id()) committer.detach();
    else committer.join();
}

void DrawPipeline::ReleaseRetired() {
    std::vector<Item> done;
    std::lock_guard lock(mutex);
    std::swap(done, retired);
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    const bool finisherBusy = DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread();
    if ((finisherBusy || Busy()) && GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    if (finisherBusy) DrawFinisher::Get().Drain();
    if (!Busy()) return;
    const auto start = std::chrono::steady_clock::now();
    SpinUntil(HandoffSpinWindow() * 2, [&] { return !Busy(); });
    std::unique_lock lock(mutex);
    ++idleWaiters;
    waitCounted(idle, lock, idleNotifies, [&] { return items.empty(); });
    --idleWaiters;
    const auto index = static_cast<std::size_t>(reason);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += elapsedNs(start);
    rethrowFailure();
}

std::uint64_t DrawPipeline::DrainRequestsByOpcode(std::uint32_t opcode) {
    return Queue0().drainRequests[std::min<std::uint32_t>(opcode, 0x100)].load(std::memory_order_relaxed);
}

bool DrawPipeline::HeldReads(std::uint64_t address, std::size_t bytes) {
    static const bool unordered = std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") != nullptr;
    if (bytes == 0 || unordered) return false;
    const auto end = address + bytes;
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    heldReadQueries.fetch_add(1, std::memory_order_relaxed);
    for (const auto& item : pipeline.items) {
        for (const auto& [begin, limit] : item.reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

std::uint64_t DrawPipeline::HeldReadQueries() {
    return heldReadQueries.load(std::memory_order_relaxed);
}

void DrawPipeline::ReleaseRunningReads() {
    std::lock_guard lock(mutex);
    if (!items.empty()) items.front().reads.clear();
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    if (DrawFinisher::Overlap()) {
        std::uint64_t held = 0;
        const auto lookup = DrawFinisher::Get().PendingLabel(address, bytes, held);
        if (lookup == DrawFinisher::LabelLookup::Value) return held;
        if (lookup == DrawFinisher::LabelLookup::Unknown) return std::nullopt;
    }
    std::uint64_t value = 0;
    if (!QueuedBytes(address, std::as_writable_bytes(std::span(&value, 1)).first(bytes))) return std::nullopt;
    return value;
}

void DrawPipeline::countWrites(const std::vector<Range>& writes, bool added) {
    for (const auto& range : writes) {
        const auto found = std::find_if(queuedWrites.begin(), queuedWrites.end(), [&](const auto& entry) { return entry.first == range; });
        if (added) {
            if (found != queuedWrites.end()) ++found->second;
            else queuedWrites.emplace_back(range, 1u);
        } else if (found != queuedWrites.end() && --found->second == 0) {
            *found = queuedWrites.back();
            queuedWrites.pop_back();
        }
    }
    static const bool bounded = FinisherWaitQueue0() || FinisherQueuedBytes() || std::getenv("APS5_VERIFY_FINISHER_READS") != nullptr;
    if (!bounded) return;
    auto low = ~std::uint64_t{0};
    std::uint64_t high = 0;
    for (const auto& [range, count] : queuedWrites) {
        low = std::min(low, range.first);
        high = std::max(high, range.second);
    }
    queuedLow.store(low, std::memory_order_release);
    queuedHigh.store(high, std::memory_order_release);
}

DrawPipeline::Watch DrawPipeline::WatchWrites() {
    if (!Busy()) return {Retired(), 0, {}};
    std::lock_guard lock(mutex);
    Watch watch{retiredItems.load(std::memory_order_relaxed), items.size(), {}};
    for (const auto& item : items) watch.writes.insert(watch.writes.end(), item.writes.begin(), item.writes.end());
    return watch;
}

bool DrawPipeline::WrittenSince(const Watch& watch, const std::vector<Range>& reads) {
    const auto overlaps = [&reads](const std::vector<Range>& writes) {
        return std::any_of(reads.begin(), reads.end(), [&writes](const Range& read) {
            return std::any_of(writes.begin(), writes.end(), [&read](const Range& write) { return read.first < write.second && write.first < read.second; });
        });
    };
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlapsAny(reads)) return true;
    std::lock_guard lock(mutex);
    if (std::any_of(items.begin(), items.end(), [&](const Item& item) { return overlaps(item.writes); })) return true;
    const auto retiredNow = retiredItems.load(std::memory_order_relaxed);
    if (retiredNow == watch.retired) return false;
    return retiredNow - watch.retired > watch.items || overlaps(watch.writes);
}

bool DrawPipeline::Busy() const {
    return outstanding.load(std::memory_order_acquire) != 0 || (finisherProducer && DrawFinisher::Busy() && DrawFinisher::Overlap());
}

bool DrawPipeline::OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlapsAny(ranges)) return true;
    std::lock_guard lock(mutex);
    for (const auto& [range, count] : queuedWrites) {
        for (const auto& [address, end] : ranges) {
            if (address < range.second && range.first < end) return true;
        }
    }
    return false;
}

bool DrawPipeline::FinisherWaitQueue0() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FINISHER_WAIT_QUEUE0");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool DrawPipeline::FinisherQueuedBytes() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FINISHER_QUEUED_BYTES");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

DrawPipeline::QueuedWrite DrawPipeline::FirstQueuedWrite(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    if (address >= queuedHigh.load(std::memory_order_acquire) || queuedLow.load(std::memory_order_acquire) >= end) return {};
    std::lock_guard lock(mutex);
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        for (const auto& range : item.writes) {
            if (address < range.second && range.first < end) return {true, item.draw, item.opcode, item.labelAddress, retiredItems.load(std::memory_order_relaxed) + index, index, range};
        }
    }
    return {};
}

bool DrawPipeline::WaitRetired(std::span<const Range> ranges) {
    if (!DrawFinisher::OnFinisherThread()) throw std::runtime_error("draw pipeline wait for retired writes off the draw finisher");
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline wait for retired writes under the GPU lock");
    const auto low = queuedLow.load(std::memory_order_acquire), high = queuedHigh.load(std::memory_order_acquire);
    if (std::none_of(ranges.begin(), ranges.end(), [&](const Range& range) { return range.first < high && low < range.second; })) return false;
    std::unique_lock lock(mutex);
    if (thread.get_id() == std::this_thread::get_id()) throw std::runtime_error("draw pipeline wait for retired writes on the committer");
    const auto clear = [&] {
        return std::none_of(queuedWrites.begin(), queuedWrites.end(), [&](const auto& entry) {
            return std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) { return range.first < entry.first.second && entry.first.first < range.second; });
        });
    };
    if (clear()) return false;
    ++retireWaiters;
    idle.wait(lock, clear);
    --retireWaiters;
    finisherWaits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool DrawPipeline::WaitRetired(std::uint64_t address, std::size_t bytes) {
    const Range range{address, address + bytes};
    return WaitRetired(std::span(&range, 1));
}

bool DrawPipeline::QueuedBytes(std::uint64_t address, std::span<std::byte> bytes) {
    const auto end = address + bytes.size();
    if (bytes.empty() || (DrawFinisher::OnFinisherThread() && (address >= queuedHigh.load(std::memory_order_acquire) || queuedLow.load(std::memory_order_acquire) >= end))) return false;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return false;
        std::memcpy(bytes.data(), item->labelBytes.data() + (address - item->labelAddress), bytes.size());
        return true;
    }
    return false;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlaps(address, end)) return true;
    std::lock_guard lock(mutex);
    for (const auto& [range, count] : queuedWrites) {
        if (address < range.second && range.first < end) return true;
    }
    return false;
}

}
