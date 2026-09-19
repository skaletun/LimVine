/**
 * @file    JobSystem.cpp
 * @brief   Реализация пула потоков LimVine.
 */
#include "JobSystem.h"

#include <algorithm>
#include <cassert>

namespace lv {

namespace {
JobSystem* g_instance = nullptr;
thread_local std::uint32_t t_workerIndex = 0xFFFFFFFFu;
} // namespace

void JobSystem::init(std::uint32_t workerCount) {
    if (g_instance) return;
    static JobSystem sys;
    g_instance = &sys;

    if (workerCount == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workerCount = hw > 1 ? hw - 1 : 1;
    }
    sys.workerCount_ = workerCount;
    sys.stop_ = false;
    sys.workers_.reserve(workerCount);
    for (std::uint32_t i = 0; i < workerCount; ++i)
        sys.workers_.emplace_back([&, i] { sys.workerLoop(i); });
}

void JobSystem::shutdown() {
    if (!g_instance) return;
    g_instance->stop_ = true;
    g_instance->cv_.notify_all();
    for (auto& t : g_instance->workers_)
        if (t.joinable()) t.join();
    g_instance->workers_.clear();
    g_instance = nullptr;
}

JobSystem& JobSystem::instance() {
    assert(g_instance && "JobSystem::init() must be called first");
    return *g_instance;
}

bool JobSystem::ready() noexcept { return g_instance != nullptr; }

JobSystem::~JobSystem() {
    stop_ = true;
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

void JobSystem::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(globalMutex_);
        globalQueue_.push_back(std::move(job));
        ++activeJobs_;
    }
    cv_.notify_one();
}

bool JobSystem::tryPopJob(Job& out) {
    std::lock_guard<std::mutex> lock(globalMutex_);
    if (globalQueue_.empty()) return false;
    out = std::move(globalQueue_.front());
    globalQueue_.pop_front();
    return true;
}

std::uint32_t JobSystem::currentWorker() const noexcept {
    return t_workerIndex == 0xFFFFFFFFu ? workerCount_ : t_workerIndex;
}

void JobSystem::workerLoop(std::uint32_t index) {
    t_workerIndex = index;
    for (;;) {
        Job job;
        if (!tryPopJob(job)) {
            if (stop_) return;
            std::unique_lock<std::mutex> lock(globalMutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [this] {
                return stop_ || !globalQueue_.empty();
            });
            if (stop_ && globalQueue_.empty()) return;
            if (globalQueue_.empty()) continue;
            job = std::move(globalQueue_.front());
            globalQueue_.pop_front();
        }
        job();
        jobsExecuted_.fetch_add(1, std::memory_order_relaxed);
        if (activeJobs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            idleCv_.notify_all();
    }
}

void JobSystem::parallelForRange(std::size_t count, std::size_t chunkSize,
                                 const std::function<void(std::size_t, std::size_t)>& body) {
    if (count == 0) return;
    chunkSize = std::max<std::size_t>(chunkSize, 1);
    const std::size_t chunks = (count + chunkSize - 1) / chunkSize;

    // Один чанк или пул недоступен — выполняем inline (без накладных расходов).
    if (chunks == 1 || workerCount_ == 0) { body(0, count); return; }

    std::atomic<std::size_t> done{0};
    const std::size_t jobs = std::min<std::size_t>(chunks, workerCount_ + 1);
    ++activeJobs_;   // учитываем задачу, которую выполнит вызывающий поток
    for (std::size_t j = 1; j < jobs; ++j) {
        submit([&, j, chunks, count, chunkSize] {
            for (std::size_t c = j; c < chunks; c += jobs) {
                const std::size_t begin = c * chunkSize;
                const std::size_t end = std::min(count, begin + chunkSize);
                body(begin, end);
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::size_t c = 0; c < chunks; c += jobs) {
        const std::size_t begin = c * chunkSize;
        const std::size_t end = std::min(count, begin + chunkSize);
        body(begin, end);
    }
    // Дожидаемся worker'ов, помогая им из общей очереди.
    while (done.load(std::memory_order_acquire) + 1 < jobs) {
        Job stolen;
        if (tryPopJob(stolen)) {
            stolen();
            jobsExecuted_.fetch_add(1, std::memory_order_relaxed);
            if (activeJobs_.fetch_sub(1, std::memory_order_acq_rel) == 1) idleCv_.notify_all();
        } else {
            std::this_thread::yield();
        }
    }
    if (activeJobs_.fetch_sub(1, std::memory_order_acq_rel) == 1) idleCv_.notify_all();
}

void JobSystem::parallelFor(std::size_t count, std::size_t chunkSize,
                            const std::function<void(std::size_t)>& body) {
    parallelForRange(count, chunkSize, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) body(i);
    });
}

} // namespace lv
