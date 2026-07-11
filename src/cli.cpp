// Sluice command-line tool: correctness self-test + benchmark vs std::sort.
//
// Author: Alphanu1 / Ben Templaman
// Since:  2026-07-08
//
//   sluice                      run self-test, then benchmark
//   sluice --test               run self-test only  (exit 1 on failure)
//   sluice --bench              run benchmark only
//   sluice --version            print version
//   sluice --sort [--asc|--desc] n1 n2 n3 ...
//                               sort the given integers and print them
//                               (ascending is the default when no flag given)
//   sluice --first K n1 n2 n3 ...
//                               print the K smallest (ascending)
//   sluice --top K n1 n2 n3 ...
//                               print the K largest (descending)
#include "sluice.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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
    // descending: must equal the reverse of the ascending result
    for (int t = 0; t < 500; ++t) {
        std::uniform_int_distribution<int> nd(0, 400);
        size_t n = nd(rng);
        std::uniform_int_distribution<int32_t> vd(-1000000, 1000000);
        std::vector<int32_t> a(n), gold(n);
        for (auto& x : a) x = vd(rng);
        gold = a;
        std::sort(gold.begin(), gold.end(), std::greater<int32_t>());
        sluice_sort_i32_ordered(a.data(), n, SLUICE_DESCENDING);
        if (a != gold) { std::printf("  i32 desc FAIL n=%zu\n", n); return false; }
    }
    // first_n = head, top_n = tail, of the array sorted in `order`
    for (int t = 0; t < 500; ++t) {
        std::uniform_int_distribution<int> nd(1, 400);
        size_t n = nd(rng);
        size_t k = std::uniform_int_distribution<size_t>(0, n + 5)(rng);  // incl. k>n
        size_t kk = std::min(k, n);
        sluice_order ord = (t & 1) ? SLUICE_DESCENDING : SLUICE_ASCENDING;
        std::uniform_int_distribution<int32_t> vd(-1000000, 1000000);
        std::vector<int32_t> a(n);
        for (auto& x : a) x = vd(rng);
        std::vector<int32_t> s(a);
        if (ord == SLUICE_ASCENDING) std::sort(s.begin(), s.end());
        else                         std::sort(s.begin(), s.end(), std::greater<int32_t>());

        std::vector<int32_t> af(a);
        size_t wf = sluice_first_n_i32(af.data(), n, k, ord);
        if (wf != kk || !std::equal(s.begin(), s.begin() + kk, af.begin())) {
            std::printf("  first_n FAIL n=%zu k=%zu\n", n, k); return false;
        }
        std::vector<int32_t> at(a);
        size_t wt = sluice_top_n_i32(at.data(), n, k, ord);
        if (wt != kk || !std::equal(s.end() - kk, s.end(), at.begin())) {  // tail kk
            std::printf("  top_n FAIL n=%zu k=%zu\n", n, k); return false;
        }
    }

    // --- fixed boundary sizes straddling every dispatch threshold ---------
    auto vs_gold_u64 = [](std::vector<uint64_t> v) {
        std::vector<uint64_t> g(v); std::sort(g.begin(), g.end());
        sluice_sort_u64(v.data(), v.size()); return v == g;
    };
    for (size_t n : {size_t(15), size_t(16), size_t(17), size_t(31), size_t(32),
                     size_t(33), size_t(511), size_t(512), size_t(513),
                     size_t(999), size_t(1000), size_t(1001),
                     size_t(100000), size_t(1000000)}) {
        std::mt19937_64 r64(n * 2654435761u);
        // several distributions per size: full-range, bounded, dup-heavy,
        // nearly-sorted, reverse-sorted, all-equal
        for (int variant = 0; variant < 6; ++variant) {
            std::vector<uint64_t> v(n);
            for (size_t i = 0; i < n; ++i) {
                switch (variant) {
                    case 0: v[i] = r64(); break;                    // full range
                    case 1: v[i] = r64() % 1000; break;             // bounded (counting)
                    case 2: v[i] = r64() % 8; break;                // duplicate-heavy
                    case 3: v[i] = i + (r64() % 4); break;          // nearly sorted
                    case 4: v[i] = n - i; break;                    // reverse sorted
                    default: v[i] = 42; break;                      // all equal
                }
            }
            if (!vs_gold_u64(v)) { std::printf("  boundary FAIL n=%zu v=%d\n", n, variant); return false; }
        }
    }

    // --- adversarial magnitudes: values where double() collides ----------
    {
        const uint64_t P63 = (1ull << 63), P53 = (1ull << 53), UMAX = ~0ull;
        std::vector<std::vector<uint64_t>> adv = {
            {P63, P63 + 1}, {P63 - 1, P63, P63 + 1, P63 + 1023},
            {P53, P53 + 1, P53 + 2}, {UMAX, UMAX - 1, UMAX - 512},
            {0, UMAX, P63, P53}, {0, 1, UMAX - 1, UMAX},
        };
        for (auto& base : adv)
            for (int reps = 1; reps <= 128; reps *= 2) {
                std::vector<uint64_t> v;
                for (int r = 0; r < reps; ++r) v.insert(v.end(), base.begin(), base.end());
                if (!vs_gold_u64(v)) { std::printf("  adversarial u64 FAIL sz=%zu\n", v.size()); return false; }
            }
    }

    // --- signed extremes: negatives must order below positives -----------
    {
        std::vector<int64_t> ext = {INT64_MIN, INT64_MAX, 0, -1, 1,
                                    INT64_MIN + 1, INT64_MAX - 1, INT64_MIN + 512};
        for (int reps = 1; reps <= 128; reps *= 2) {
            std::vector<int64_t> v;
            for (int r = 0; r < reps; ++r) v.insert(v.end(), ext.begin(), ext.end());
            std::vector<int64_t> g(v); std::sort(g.begin(), g.end());
            sluice_sort_i64(v.data(), v.size());
            if (v != g) { std::printf("  i64 extreme FAIL sz=%zu\n", v.size()); return false; }
        }
    }

    // --- float / double: value-sort must match std::sort (non-NaN) -------
    auto rand_f32 = [&]() { uint32_t b;
        do { b = static_cast<uint32_t>(rng()); } while (((b >> 23) & 0xFFu) == 0xFFu && (b & 0x7FFFFFu) != 0);
        float f; std::memcpy(&f, &b, sizeof f); return f; };
    auto rand_f64 = [&]() { uint64_t b; std::mt19937_64 r2(rng());
        do { b = r2(); } while (((b >> 52) & 0x7FFull) == 0x7FFull && (b & 0xFFFFFFFFFFFFFull) != 0);
        double d; std::memcpy(&d, &b, sizeof d); return d; };
    const float  fspec[] = {0.0f,-0.0f,1.0f,-1.0f,INFINITY,-INFINITY,FLT_MIN,-FLT_MIN,
                            FLT_MAX,-FLT_MAX,std::numeric_limits<float>::denorm_min(),3.14f,-2.5f};
    for (size_t n : {size_t(2), size_t(16), size_t(17), size_t(511), size_t(512),
                     size_t(513), size_t(5000)}) {
        std::vector<float> a(n);
        for (size_t i = 0; i < n; ++i) a[i] = (i < sizeof(fspec)/sizeof(float)) ? fspec[i] : rand_f32();
        std::vector<float> g(a); std::sort(g.begin(), g.end());
        std::vector<float> asc(a); sluice_sort_f32(asc.data(), n);
        for (size_t i = 0; i < n; ++i) if (asc[i] != g[i]) { std::printf("  f32 FAIL n=%zu i=%zu\n", n, i); return false; }
        std::vector<float> dsc(a); sluice_sort_f32_ordered(dsc.data(), n, SLUICE_DESCENDING);
        for (size_t i = 0; i < n; ++i) if (dsc[i] != g[n-1-i]) { std::printf("  f32 desc FAIL n=%zu\n", n); return false; }
    }
    for (size_t n : {size_t(2), size_t(512), size_t(513), size_t(5000)}) {
        std::vector<double> a(n);
        for (size_t i = 0; i < n; ++i) a[i] = rand_f64();
        std::vector<double> g(a); std::sort(g.begin(), g.end());
        sluice_sort_f64(a.data(), n);
        for (size_t i = 0; i < n; ++i) if (a[i] != g[i]) { std::printf("  f64 FAIL n=%zu\n", n); return false; }
    }
    // NaN: not comparable, but Sluice must produce a permutation without UB
    {
        float nan = std::numeric_limits<float>::quiet_NaN();
        std::vector<float> a = {nan, 1.0f, -1.0f, nan, 0.0f, INFINITY, -nan, 2.0f};
        auto bits = [](const std::vector<float>& v){ std::vector<uint32_t> b(v.size());
            for (size_t i=0;i<v.size();++i) { std::memcpy(&b[i],&v[i],4); }
            std::sort(b.begin(),b.end()); return b; };
        std::vector<uint32_t> before = bits(a);
        sluice_sort_f32(a.data(), a.size());
        if (bits(a) != before) { std::printf("  f32 NaN not a permutation\n"); return false; }
    }

    // --- unified dispatcher: results match the specialized paths ----------
    {
        std::mt19937 r(99);
        for (int t = 0; t < 300; ++t) {
            size_t n = r() % 2000;
            std::vector<int32_t> base(n);
            for (auto& x : base) x = static_cast<int32_t>(r()) ;
            std::vector<int32_t> g(base); std::sort(g.begin(), g.end());
            // sort all, ascending, no stats
            std::vector<int32_t> a(base);
            if (sluice_sort(SLUICE_I32, a.data(), n, 0, nullptr, 0, nullptr, nullptr) != SLUICE_OK ||
                a != g) { std::printf("  unified i32 sort FAIL n=%zu\n", n); return false; }
            // first k via select>0
            size_t k = n ? (r() % n) : 0;
            std::vector<int32_t> f(base);
            sluice_order asc = SLUICE_ASCENDING;
            sluice_sort(SLUICE_I32, f.data(), n, static_cast<ptrdiff_t>(k), &asc, 0, nullptr, nullptr);
            if (!std::equal(g.begin(), g.begin() + std::min(k,n), f.begin())) { std::printf("  unified first FAIL\n"); return false; }
            // top k via select<0
            std::vector<int32_t> tp(base);
            sluice_sort(SLUICE_I32, tp.data(), n, -static_cast<ptrdiff_t>(k), &asc, 0, nullptr, nullptr);
            if (!std::equal(g.end() - std::min(k,n), g.end(), tp.begin())) { std::printf("  unified top FAIL\n"); return false; }
        }
        // stats path: already-sorted detection + correctness
        std::vector<uint32_t> srt(1000); for (size_t i=0;i<srt.size();++i) srt[i]=static_cast<uint32_t>(i);
        sluice_stats s;
        if (sluice_sort(SLUICE_U32, srt.data(), srt.size(), 0, nullptr, 1, &s, nullptr) != SLUICE_OK) { std::printf("  stats rc FAIL\n"); return false; }
        if (!s.already_sorted || std::strcmp(s.algorithm, "already sorted") != 0) { std::printf("  stats already_sorted FAIL\n"); return false; }
        // stats with duplicates + bounded range -> counting
        std::vector<uint32_t> dup(50000); std::mt19937 r2(7); for (auto& x : dup) x = r2() % 100;
        std::vector<uint32_t> dgold(dup); std::sort(dgold.begin(), dgold.end());
        sluice_sort(SLUICE_U32, dup.data(), dup.size(), 0, nullptr, 1, &s, nullptr);
        if (dup != dgold || s.duplicate_pct < 99.0 || std::strcmp(s.algorithm,"counting") != 0) { std::printf("  stats counting FAIL\n"); return false; }
        // error paths
        if (sluice_sort(static_cast<sluice_dtype>(42), nullptr, 0, 0, nullptr, 0, nullptr, nullptr) != SLUICE_ERR_TYPE) { std::printf("  bad-type FAIL\n"); return false; }
        if (sluice_sort(SLUICE_U32, dup.data(), 1, 0, nullptr, 1, nullptr, nullptr) != SLUICE_ERR_NULL) { std::printf("  null-stats FAIL\n"); return false; }
    }

    // --- float first_n / top_n -------------------------------------------
    {
        std::vector<float> base = {3.14f,-2.5f,0.0f,-1.0f,2.71f,9.9f,-8.1f,4.0f};
        std::vector<float> g(base); std::sort(g.begin(), g.end());
        std::vector<float> f(base); size_t w = sluice_first_n_f32(f.data(), f.size(), 3, SLUICE_ASCENDING);
        if (w != 3 || !std::equal(g.begin(), g.begin()+3, f.begin())) { std::printf("  f32 first_n FAIL\n"); return false; }
        std::vector<float> tp(base); w = sluice_top_n_f32(tp.data(), tp.size(), 3, SLUICE_ASCENDING);
        if (w != 3 || !std::equal(g.end()-3, g.end(), tp.begin())) { std::printf("  f32 top_n FAIL\n"); return false; }
    }

    // --- custom dispatch config ------------------------------------------
    {
        // config_init fills defaults
        sluice_config c; sluice_config_init(&c);
        if (c.insertion_limit != 16 || c.interpolation_limit != 512 ||
            c.interpolation_skew != 32 || c.counting_load != 4 || c.counting_cap != (1u<<21)) {
            std::printf("  config defaults FAIL\n"); return false;
        }
        std::mt19937 r(2024);
        // custom thresholds must still sort correctly across sizes/types
        for (int t = 0; t < 200; ++t) {
            size_t n = r() % 3000;
            std::vector<uint32_t> a(n); for (auto& x : a) x = r();
            std::vector<uint32_t> g(a); std::sort(g.begin(), g.end());
            sluice_config cfg{};
            cfg.interpolation_limit = 768;   // raised past the default 512 (heap scratch)
            cfg.counting_load = 8;
            if (sluice_sort(SLUICE_U32, a.data(), n, 0, nullptr, 0, nullptr, &cfg) != SLUICE_OK || a != g) {
                std::printf("  config sort FAIL n=%zu\n", n); return false; }
        }
        // a raised interpolation_limit should actually route n=700 to interpolation
        {
            std::vector<uint32_t> a(700); for (auto& x : a) x = r();
            sluice_config cfg{}; cfg.interpolation_limit = 1024;
            sluice_stats s;
            sluice_sort(SLUICE_U32, a.data(), a.size(), 0, nullptr, 1, &s, &cfg);
            if (std::strcmp(s.algorithm, "interpolation") != 0) { std::printf("  config interp route FAIL (%s)\n", s.algorithm); return false; }
        }
        // a zeroed-but-for-one-field config leaves other knobs at default
        {
            std::vector<uint32_t> a(50000); std::mt19937 rr(5); for (auto& x : a) x = rr() % 5000;
            std::vector<uint32_t> g(a); std::sort(g.begin(), g.end());
            sluice_config cfg{}; cfg.counting_cap = 1u << 10;  // tiny cap -> forces radix
            sluice_stats s;
            sluice_sort(SLUICE_U32, a.data(), a.size(), 0, nullptr, 1, &s, &cfg);
            if (a != g || std::strcmp(s.algorithm, "radix") != 0) { std::printf("  config cap FAIL (%s)\n", s.algorithm); return false; }
        }
    }

    // --- parallel radix: result must equal the sequential sort -----------
    {
        std::mt19937_64 r(2025);
        // sizes above and around a lowered parallel_min, several thread counts,
        // several distributions; each must match std::sort exactly.
        for (int threads : {2, 3, 4, 8}) {
            for (int shape = 0; shape < 4; ++shape) {
                size_t n = 300000 + (r() % 200000);
                std::vector<uint64_t> a(n);
                for (size_t i = 0; i < n; ++i) {
                    switch (shape) {
                        case 0: a[i] = r(); break;                 // full range
                        case 1: a[i] = r() % 1000000; break;       // moderate range
                        case 2: a[i] = r() % 16; break;            // heavy dup (skewed buckets)
                        default: a[i] = (r() & 0xFFull) << 56;     // only top byte varies (bucket skew)
                    }
                }
                std::vector<uint64_t> g(a); std::sort(g.begin(), g.end());
                sluice_config cfg{}; cfg.max_threads = threads; cfg.parallel_min = 200000;
                cfg.counting_cap = 1;  // force the radix path so parallel actually engages
                sluice_stats s;
                sluice_sort(SLUICE_U64, a.data(), n, 0, nullptr, 1, &s, &cfg);
                if (a != g) { std::printf("  parallel u64 FAIL threads=%d shape=%d n=%zu\n", threads, shape, n); return false; }
                if (std::strcmp(s.algorithm, "radix") == 0 && s.threads_used != threads) {
                    std::printf("  parallel threads_used=%d expected=%d\n", s.threads_used, threads); return false;
                }
            }
        }
        // parallel + descending + top-N still correct
        {
            size_t n = 400000; std::vector<int32_t> a(n);
            for (auto& x : a) x = static_cast<int32_t>(r());
            std::vector<int32_t> g(a); std::sort(g.begin(), g.end(), std::greater<int32_t>());
            sluice_config cfg{}; cfg.max_threads = 4; cfg.parallel_min = 100000;
            sluice_order desc = SLUICE_DESCENDING;
            sluice_sort(SLUICE_I32, a.data(), n, 0, &desc, 0, nullptr, &cfg);
            if (a != g) { std::printf("  parallel desc FAIL\n"); return false; }
        }
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

// --------------------------------------------- custom-array sort / select
// Unified command: flags may appear in any order; no --sort keyword required.
//   [--asc|--desc]  set direction (default ascending)
//   --first K       keep the first K of the sorted result (the head)
//   --top   K       keep the last  K of the sorted result (the tail)
//   n1 n2 ...        the integers to sort
// With no --first/--top, the whole sorted array is printed.
static int run_select(int argc, char** argv) {
    sluice_order order = SLUICE_ASCENDING;
    enum { OP_SORT, OP_FIRST, OP_TOP } op = OP_SORT;
    size_t k = 0;
    std::vector<int64_t> nums;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (std::strcmp(a, "--asc")  == 0) { order = SLUICE_ASCENDING;  continue; }
        else if (std::strcmp(a, "--desc") == 0) { order = SLUICE_DESCENDING; continue; }
        else if (std::strcmp(a, "--sort") == 0) { op = OP_SORT;              continue; }
        else if (std::strcmp(a, "--first") == 0 || std::strcmp(a, "--top") == 0) {
            op = (a[2] == 'f') ? OP_FIRST : OP_TOP;
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sluice %s: missing K\n", a);
                return 1;
            }
            char* end = nullptr;
            long long kll = std::strtoll(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || kll < 0) {
                std::fprintf(stderr, "sluice %s: K must be a non-negative integer, got '%s'\n", a, argv[i]);
                return 1;
            }
            k = static_cast<size_t>(kll);
            continue;
        }
        char* end = nullptr;
        long long v = std::strtoll(a, &end, 10);
        if (end == a || *end != '\0') {
            std::fprintf(stderr, "sluice: skipping non-integer '%s'\n", a);
            continue;
        }
        nums.push_back(static_cast<int64_t>(v));
    }

    if (nums.empty()) {
        std::fprintf(stderr,
            "usage: sluice [--asc|--desc] [--first K | --top K] n1 n2 n3 ...\n");
        return 1;
    }

    size_t n = nums.size(), count = n;
    if      (op == OP_FIRST) count = sluice_first_n_i64(nums.data(), n, k, order);
    else if (op == OP_TOP)   count = sluice_top_n_i64(nums.data(), n, k, order);
    else                     sluice_sort_i64_ordered(nums.data(), n, order);

    for (size_t i = 0; i < count; ++i)
        std::printf("%s%lld", i ? " " : "", static_cast<long long>(nums[i]));
    std::printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "--version") == 0) { std::printf("%s\n", sluice_version()); return 0; }

    // Anything that isn't a diagnostic mode is treated as a sort/select command.
    bool diagnostic = (argc == 1) || std::strcmp(mode, "--test") == 0
                                  || std::strcmp(mode, "--bench") == 0;
    if (!diagnostic) return run_select(argc, argv);

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
