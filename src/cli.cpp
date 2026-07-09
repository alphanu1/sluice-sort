// Sluice command-line tool: correctness self-test + benchmark vs std::sort.
//   sluice           run self-test, then benchmark
//   sluice --test    run self-test only  (exit 1 on failure)
//   sluice --bench   run benchmark only
//   sluice --version print version
#include "sluice.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using Clock = std::chrono::steady_clock;
static volatile uint64_t g_sink = 0;

// ----------------------------------------------------------- correctness
static bool self_test() {
    std::mt19937 rng(20260709);
    auto check = [&](size_t n, uint32_t lo, uint32_t hi) -> bool {
        std::uniform_int_distribution<uint32_t> d(lo, hi);
        std::vector<uint32_t> a(n);
        for (auto& x : a) x = d(rng);
        std::vector<uint32_t> gold(a);
        std::sort(gold.begin(), gold.end());
        sluice_sort_u32(a.data(), n);
        return a == gold;
    };
    // sizes straddling every dispatch threshold, ranges hitting every path
    for (int t = 0; t < 2000; ++t) {
        std::uniform_int_distribution<int> nd(0, 400);
        size_t n = nd(rng);
        uint32_t hi = (rng() & 3) == 0 ? 7u                       // dup-heavy
                    : (rng() & 1)      ? 100000u                   // counting
                                       : 0xFFFFFFFFu;              // radix
        if (!check(n, 0, hi)) { std::printf("  u32 FAIL n=%zu hi=%u\n", n, hi); return false; }
    }
    // edge cases
    for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(31), size_t(32), size_t(33)}) {
        std::vector<uint32_t> a(n, 5u);
        sluice_sort_u32(a.data(), n);
    }
    // signed correctness (negatives must order below positives)
    for (int t = 0; t < 1000; ++t) {
        std::uniform_int_distribution<int> nd(0, 400);
        size_t n = nd(rng);
        std::uniform_int_distribution<int32_t> vd(-1000000, 1000000);
        std::vector<int32_t> a(n);
        for (auto& x : a) x = vd(rng);
        std::vector<int32_t> gold(a);
        std::sort(gold.begin(), gold.end());
        sluice_sort_i32(a.data(), n);
        if (a != gold) { std::printf("  i32 FAIL n=%zu\n", n); return false; }
    }
    // 64-bit
    for (int t = 0; t < 500; ++t) {
        std::uniform_int_distribution<int> nd(0, 400);
        size_t n = nd(rng);
        std::uniform_int_distribution<uint64_t> vd(0, ~0ull);
        std::vector<uint64_t> a(n);
        for (auto& x : a) x = vd(rng);
        std::vector<uint64_t> gold(a);
        std::sort(gold.begin(), gold.end());
        sluice_sort_u64(a.data(), n);
        if (a != gold) { std::printf("  u64 FAIL n=%zu\n", n); return false; }
    }
    return true;
}

// ------------------------------------------------------------- benchmark
// A pool of many DISTINCT arrays per size. Timing the whole pool defeats the
// classic microbenchmark trap where re-sorting one identical array lets the
// CPU's branch predictor memorise the comparison sequence and flatter a
// comparison sort. Real workloads sort varied data; so does this.
struct Pool { std::vector<uint32_t> flat; int n; int count; };

static Pool make_pool(std::mt19937& rng, int n, long budget, uint32_t hi, bool presorted) {
    int count = (int)std::max(1L, budget / n);
    Pool p{std::vector<uint32_t>((size_t)count * n), n, count};
    std::uniform_int_distribution<uint32_t> d(0, hi);
    for (auto& x : p.flat) x = d(rng);
    if (presorted)
        for (int i = 0; i < count; ++i) std::sort(p.flat.begin()+ (size_t)i*n, p.flat.begin()+(size_t)(i+1)*n);
    return p;
}

template <class F>
static double bench_pool(const Pool& p, F fn, int repeat) {
    std::vector<uint32_t> work(p.flat.size());
    double best = 1e30;
    for (int r = 0; r < repeat; ++r) {
        work = p.flat;                                   // fresh unsorted copy
        auto t0 = Clock::now();
        for (int i = 0; i < p.count; ++i) fn(work.data() + (size_t)i * p.n, p.n);
        auto t1 = Clock::now();
        uint64_t cs = 0; for (int i = 0; i < p.count; ++i) cs += work[(size_t)i * p.n]; g_sink += cs;
        double per = std::chrono::duration<double>(t1 - t0).count() / p.count;
        if (per < best) best = per;
    }
    return best;
}

static void row(const char* label, const Pool& p, int repeat) {
    double ts = bench_pool(p, [](uint32_t* a, int m){ std::sort(a, a + m); }, repeat);
    double tl = bench_pool(p, [](uint32_t* a, int m){ sluice_sort_u32(a, (size_t)m); }, repeat);
    auto f = [](double s, char* b){ if (s*1e6 < 1000) std::snprintf(b,32,"%8.2f us",s*1e6);
                                    else               std::snprintf(b,32,"%8.2f ms",s*1e3); };
    char b1[32], b2[32]; f(ts,b1); f(tl,b2);
    std::printf("  %-26s std::sort %s   sluice %s   %5.2fx %s\n",
                label, b1, b2, ts/tl, tl<ts ? "faster" : "slower");
}

static void benchmark() {
    std::mt19937 rng(1);
    const long B = 2000000;  // ~elements per pool, so every size does equal work
    std::printf("benchmark (pool of distinct arrays per size; realistic, not\n"
                "branch-prediction-gamed; every sample sorts a fresh copy)\n");
    row("n=50    uniform 32-bit", make_pool(rng, 50,      B, 0xFFFFFFFFu, false), 7);
    row("n=200   uniform 32-bit", make_pool(rng, 200,     B, 0xFFFFFFFFu, false), 7);
    row("n=1000  uniform 32-bit", make_pool(rng, 1000,    B, 0xFFFFFFFFu, false), 7);
    row("n=100k  uniform 32-bit", make_pool(rng, 100000,  B, 0xFFFFFFFFu, false), 7);
    row("n=1M    uniform 32-bit", make_pool(rng, 1000000, B, 0xFFFFFFFFu, false), 7);
    row("n=1M    bounded <1000",  make_pool(rng, 1000000, B, 999u,        false), 7);
    row("n=1M    already sorted", make_pool(rng, 1000000, B, 0xFFFFFFFFu, true),  7);
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "--version") == 0) { std::printf("%s\n", sluice_version()); return 0; }

    if (std::strcmp(mode, "--bench") != 0) {
        std::printf("%s — self-test\n", sluice_version());
        bool ok = self_test();
        std::printf("  correctness vs std::sort: %s\n\n", ok ? "PASS" : "FAIL");
        if (!ok) return 1;
        if (std::strcmp(mode, "--test") == 0) return 0;
    }
    benchmark();
    return 0;
}
