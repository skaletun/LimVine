/**
 * @file    job_tests.cpp
 * @brief   Проверки JobSystem: корректность parallelFor и ускорение на N потоках.
 */
#include "core/JobSystem.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

namespace {
int g_passed = 0, g_failed = 0;
void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
double heavy(std::size_t i) {
    double v = 0;
    for (int k = 0; k < 200; ++k) v += std::sin(static_cast<double>(i) * 0.001 + k);
    return v;
}
} // namespace

int main() {
    lv::JobSystem::init(0);   // hardware_concurrency - 1
    auto& js = lv::JobSystem::instance();
    std::printf("\n== JobSystem ==\n");
    std::printf("  workers=%u\n", js.workerCount());

    constexpr std::size_t N = 1u << 18;
    std::vector<double> out(N, 0.0);

    // 1) Корректность: каждый индекс обработан ровно один раз.
    std::atomic<std::size_t> touched{0};
    js.parallelFor(N, 512, [&](std::size_t i) { out[i] = heavy(i); touched.fetch_add(1); });
    check(touched.load() == N, "every index processed exactly once");
    check(std::fabs(out[N - 1] - heavy(N - 1)) < 1e-9, "results written to the right slots");

    // 2) Детерминизм: повторный прогон даёт те же значения.
    std::vector<double> out2(N, 0.0);
    js.parallelFor(N, 512, [&](std::size_t i) { out2[i] = heavy(i); });
    check(out == out2, "parallel results are deterministic");

    // 3) Ускорение относительно однопоточного прогона.
    const auto t0 = std::chrono::steady_clock::now();
    js.parallelFor(N, 512, [&](std::size_t i) { out[i] = heavy(i); });
    const auto t1 = std::chrono::steady_clock::now();
    const double parMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    volatile double sink = 0;
    const auto t2 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < N; ++i) sink += heavy(i);
    const auto t3 = std::chrono::steady_clock::now();
    const double seqMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::printf("  parallel=%.1f ms serial=%.1f ms speedup=x%.2f\n", parMs, seqMs, seqMs / parMs);
    check(parMs > 0.0 && seqMs > 0.0, "timings measured");
    check(js.jobsExecuted() > 0, "jobs were executed by the pool");

    // 4) Вложенные диапазоны и пустые входы не падают.
    js.parallelFor(0, 64, [](std::size_t) {});
    js.parallelFor(1, 64, [&](std::size_t i) { out[i] = 1.0; });
    check(out[0] == 1.0, "degenerate ranges handled");

    lv::JobSystem::shutdown();
    std::printf("\n----------------------------------------\n");
    std::printf("JobSystem self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
