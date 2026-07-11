// ==========================================================================
// Sluice — an adaptive numeric sorting engine.
//
// Author: Alphanu1 / Ben Templaman
// Since:  2026-07-08
//
// One templated core sorts UNSIGNED integer keys, dispatching among insertion,
// interpolation (Flashsort), counting, and radix by input shape. Every public
// element type is mapped onto that core by an order-preserving key transform,
// then mapped back:
//   * unsigned integers -> used directly
//   * signed integers   -> flip the sign bit
//   * float / double    -> IEEE-754 order-preserving key (memcpy for bit access)
// so the radix and counting logic stay uniform and branch-free, and one engine
// serves u32 / i32 / u64 / i64 / float / double.
// ==========================================================================
#include "sluice.h"

#include <algorithm>   // std::sort, std::copy
#include <atomic>      // parallel bucket work-stealing
#include <chrono>      // stats timing
#include <cstring>     // std::memcpy
#include <new>         // std::bad_alloc
#include <thread>      // parallel radix workers
#include <vector>

namespace {

// --- tuning knobs -------------------------------------------------------
// Compiled-in defaults. These are also the values the specialized fast
// functions always use (no config path), so they stay branch-for-branch as
// today.
constexpr size_t   INSERTION_MAX  = 16;        // n < this: insertion sort
constexpr size_t   INTERP_MAX     = 512;       // n <= this: interpolation place
constexpr int      INTERP_SKEW    = 32;        // bail to radix if a bucket
                                               //   exceeds this (bounds repair
                                               //   work to <= 32n, defuses O(n^2))
constexpr uint64_t COUNTING_LOAD  = 4;         // counting if range <= LOAD*n
constexpr uint64_t COUNTING_CAP   = 1ull << 21;// ...and range <= ~2.1M slots

// Hard ceiling on interpolation_max: the interp path uses fixed stack scratch
// sized to this, so a user-supplied interpolation_max is clamped here.
constexpr size_t   INTERP_CAP     = 4096;

// Runtime thresholds threaded through the engine when a config is supplied.
// Default-constructed == the compiled-in defaults above.
struct Thresholds {
    size_t   insertion_max  = INSERTION_MAX;
    size_t   interp_max     = INTERP_MAX;
    int      interp_skew    = INTERP_SKEW;
    uint64_t counting_load  = COUNTING_LOAD;
    uint64_t counting_cap   = COUNTING_CAP;
    int      max_threads    = 1;         // 0/1 = sequential
    size_t   parallel_min   = 262144;    // only parallelize radix when n >= this
};


// --- insertion sort (tiny arrays; also the base case) -------------------
template <class U>
void insertion(U* a, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        U key = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > key) { a[j] = a[j - 1]; --j; }
        a[j] = key;
    }
}

// --- interpolation placement sort (stack-only, for n <= INTERP_MAX) -----
// This is Flashsort (Karl-Dietrich Neubert, "The Flashsort1 Algorithm",
// Dr. Dobb's Journal 23(2), 1998) — an in-place histogram/bucket sort. Each
// element is classified to a bucket by linear interpolation over [min,max],
// counts are accumulated into offsets, elements are placed, and a final
// insertion pass repairs local disorder. See README "References".
//
// On varied small arrays it beats introsort ~2-4x because it is nearly branch-
// free where a comparison sort suffers branch mispredictions.
//
// Returns true if it sorted the array. Returns false (having touched nothing
// destructively past a harmless scan) when it detects skew — a bucket larger
// than INTERP_SKEW — so the caller can hand off to radix. That guard bounds
// the insertion-repair work to <= INTERP_SKEW * n, defusing the O(n^2) tail
// that makes interpolation sorts dangerous on adversarial input.
//
// Key precision loss for wide U only affects placement; the insertion pass
// repairs any local disorder, so a successful result is always correct.
// Exact bucket index = (off * (n-1)) / range, with off in [0, range] and
// range >= 1, so the result is always in [0, n-1]. Uses 128-bit integers where
// available (all mainstream 64-bit GCC/Clang, incl. AArch64 and MinGW); for
// <=32-bit keys uint64 already suffices. The only fallback that touches
// floating point is 64-bit keys on a platform without 128-bit ints — and even
// then `range` was formed by integer subtraction so it is >= 1, meaning
// (double)range >= 1 and there is NO division by zero. The bucket is only an
// estimate anyway; interp_small's repair pass and skew guard keep results
// correct regardless of rounding.
template <class U>
inline int interp_bucket(U off, unsigned nm1, U range) {
#if defined(__SIZEOF_INT128__)
    return static_cast<int>((static_cast<unsigned __int128>(off) * nm1) / range);
#else
    if (sizeof(U) <= sizeof(uint32_t))
        return static_cast<int>((static_cast<uint64_t>(off) * nm1) / range);
    double k = static_cast<double>(off) * static_cast<double>(nm1)
             / static_cast<double>(range);
    int ki = static_cast<int>(k);
    return ki < 0 ? 0 : (ki > static_cast<int>(nm1) ? static_cast<int>(nm1) : ki);
#endif
}

template <class U>
bool interp_small(U* a, int n, int skew) {
    if (n < 2) return true;
    U mn = a[0], mx = a[0];
    for (int i = 1; i < n; ++i) { U x = a[i]; if (x < mn) mn = x; else if (x > mx) mx = x; }
    if (mn == mx) return true;
    const U range = static_cast<U>(mx - mn);         // exact integer diff, >= 1
    const unsigned nm1 = static_cast<unsigned>(n - 1);

    // Scratch stays on the stack for the default band (n <= INTERP_MAX); a
    // config that raises interpolation_max up to INTERP_CAP spills to the heap
    // (and bails to radix if that allocation fails). The fast path is unchanged.
    uint16_t key_s[INTERP_MAX]; int cnt_s[INTERP_MAX + 1]; U out_s[INTERP_MAX];
    std::vector<uint16_t> key_h; std::vector<int> cnt_h; std::vector<U> out_h;
    uint16_t* key = key_s; int* cnt = cnt_s; U* out = out_s;
    if (n > static_cast<int>(INTERP_MAX)) {
        try { key_h.resize(static_cast<size_t>(n)); cnt_h.resize(static_cast<size_t>(n) + 1);
              out_h.resize(static_cast<size_t>(n)); }
        catch (const std::bad_alloc&) { return false; }
        key = key_h.data(); cnt = cnt_h.data(); out = out_h.data();
    }

    for (int i = 0; i < n; ++i) cnt[i] = 0;
    int maxbucket = 0;
    for (int i = 0; i < n; ++i) {
        int k = interp_bucket<U>(static_cast<U>(a[i] - mn), nm1, range);  // in [0, n-1]
        key[i] = static_cast<uint16_t>(k);
        int c = ++cnt[k];
        if (c > maxbucket) maxbucket = c;
    }
    if (maxbucket > skew) return false;        // skewed -> let radix handle it
    int sum = 0;
    for (int i = 0; i < n; ++i) { int c = cnt[i]; cnt[i] = sum; sum += c; }
    for (int i = 0; i < n; ++i) out[cnt[key[i]]++] = a[i];
    for (int i = 1; i < n; ++i) {              // local repair (nearly sorted)
        U v = out[i]; int j = i;
        while (j > 0 && out[j - 1] > v) { out[j] = out[j - 1]; --j; }
        out[j] = v;
    }
    for (int i = 0; i < n; ++i) a[i] = out[i];
    return true;
}

// --- counting sort: values in [mn, mn+range] ----------------------------
// Counting sort (Harold H. Seward, 1954). Tally each value's frequency, then
// emit values in order — no comparisons, O(n + range). See README "References".
// Caller guarantees range+1 <= COUNTING_CAP, so the allocation is bounded.
template <class U>
void counting(U* a, size_t n, U mn, uint64_t range) {
    std::vector<size_t> cnt(static_cast<size_t>(range) + 1, 0);
    for (size_t i = 0; i < n; ++i) ++cnt[static_cast<size_t>(a[i] - mn)];
    size_t idx = 0;
    for (uint64_t v = 0; v <= range; ++v) {
        size_t c = cnt[static_cast<size_t>(v)];
        U val = static_cast<U>(mn + static_cast<U>(v));
        for (; c > 0; --c) a[idx++] = val;
    }
}

// --- LSD radix sort, base 256, sizeof(U) passes -------------------------
// Least-significant-digit radix sort — a non-comparison sort whose lineage
// traces to Hollerith's tabulating machines (1880s); see Knuth, TAOCP Vol. 3.
// Each pass is a stable counting sort on one byte; ping-pong buffers avoid
// per-pass reallocation. See README "References".
template <class U>
void radix(U* a, size_t n) {
    std::vector<U> buf(n);            // may throw bad_alloc -> caught upstream
    U* src = a;
    U* dst = buf.data();
    constexpr int passes = static_cast<int>(sizeof(U));
    for (int p = 0; p < passes; ++p) {
        const int shift = p * 8;
        size_t count[256] = {0};
        for (size_t i = 0; i < n; ++i)
            ++count[(static_cast<uint64_t>(src[i]) >> shift) & 0xFFu];
        size_t sum = 0;              // exclusive prefix sum -> bucket offsets
        for (int b = 0; b < 256; ++b) { size_t c = count[b]; count[b] = sum; sum += c; }
        for (size_t i = 0; i < n; ++i)
            dst[count[(static_cast<uint64_t>(src[i]) >> shift) & 0xFFu]++] = src[i];
        std::swap(src, dst);
    }
    if (src != a) std::memcpy(a, src, n * sizeof(U));  // even passes end in a
}

// LSD radix over the LOW sizeof(U)-1 bytes only (top byte assumed constant).
// Used to finish each MSD bucket, where every key shares the same top byte.
template <class U>
void radix_lower(U* a, size_t n) {
    if (n < 2) return;
    std::vector<U> buf(n);
    U* src = a; U* dst = buf.data();
    constexpr int passes = static_cast<int>(sizeof(U)) - 1;
    for (int p = 0; p < passes; ++p) {
        const int shift = p * 8;
        size_t count[256] = {0};
        for (size_t i = 0; i < n; ++i) ++count[(static_cast<uint64_t>(src[i]) >> shift) & 0xFFu];
        size_t sum = 0;
        for (int b = 0; b < 256; ++b) { size_t c = count[b]; count[b] = sum; sum += c; }
        for (size_t i = 0; i < n; ++i)
            dst[count[(static_cast<uint64_t>(src[i]) >> shift) & 0xFFu]++] = src[i];
        std::swap(src, dst);
    }
    if (src != a) std::memcpy(a, src, n * sizeof(U));
}

// Parallel most-significant-digit radix. One pass partitions by the top byte
// into 256 contiguous, independent buckets (concatenated in order they are
// already globally sorted — no merge). Worker threads then finish each bucket
// on its lower bytes via radix_lower, pulling buckets from a shared atomic
// counter for dynamic load balancing (handles skew). Result is identical to the
// sequential radix. Returns the number of worker threads used, or 0 on OOM
// (caller then falls back to sequential radix).
template <class U>
int parallel_radix(U* a, size_t n, int want_threads) {
    constexpr int TOPSHIFT = (static_cast<int>(sizeof(U)) - 1) * 8;
    std::vector<U> buf;
    try { buf.resize(n); } catch (const std::bad_alloc&) { return 0; }

    size_t off[257];
    {
        size_t hist[256] = {0};
        for (size_t i = 0; i < n; ++i) ++hist[(static_cast<uint64_t>(a[i]) >> TOPSHIFT) & 0xFFu];
        size_t sum = 0;
        for (int b = 0; b < 256; ++b) { off[b] = sum; sum += hist[b]; }
        off[256] = sum;
    }
    {
        size_t cur[256];
        for (int b = 0; b < 256; ++b) cur[b] = off[b];
        for (size_t i = 0; i < n; ++i) {
            int b = static_cast<int>((static_cast<uint64_t>(a[i]) >> TOPSHIFT) & 0xFFu);
            buf[cur[b]++] = a[i];
        }
    }

    int nthreads = want_threads;
    if (nthreads > 256) nthreads = 256;
    if (nthreads < 1)   nthreads = 1;

    std::atomic<int> next_bucket{0};
    auto worker = [&]() {
        for (;;) {
            int b = next_bucket.fetch_add(1, std::memory_order_relaxed);
            if (b >= 256) break;
            size_t start = off[b], len = off[b + 1] - off[b];
            if (len > 1) radix_lower(buf.data() + start, len);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads - 1));
    for (int t = 1; t < nthreads; ++t) pool.emplace_back(worker);
    worker();                                   // the calling thread is a worker too
    for (auto& th : pool) th.join();

    std::memcpy(a, buf.data(), n * sizeof(U));
    return nthreads;
}

// --- the dispatcher -----------------------------------------------------
// Optional out-parameter: when non-null, records which path actually ran, how
// many radix passes, and how many threads. Left null on the fast path.
struct core_path { const char* algorithm; int passes; int threads; };

template <class U>
void sluice_core(U* a, size_t n, core_path* path = nullptr, const Thresholds& th = Thresholds{}) {
    auto note = [&](const char* alg, int passes, int threads) { if (path) { path->algorithm = alg; path->passes = passes; path->threads = threads; } };
    note("insertion", 0, 1);
    if (n < 2) return;
    if (n < th.insertion_max) { insertion(a, n); return; }
    // small arrays: the interpolation placement sort wins here. If it detects
    // skew it returns false and we fall through to radix (n is small, so the
    // radix allocation is tiny).
    if (n <= th.interp_max && interp_small(a, static_cast<int>(n), th.interp_skew)) { note("interpolation", 0, 1); return; }

    // one scan: min, max, and "already sorted?" — cheap, high-value.
    U mn = a[0], mx = a[0];
    bool sorted = true;
    for (size_t i = 1; i < n; ++i) {
        U x = a[i];
        if (x < a[i - 1]) sorted = false;
        if (x < mn) mn = x;
        else if (x > mx) mx = x;
    }
    if (sorted) { note("already sorted", 0, 1); return; }

    const uint64_t range = static_cast<uint64_t>(mx) - static_cast<uint64_t>(mn);

    // bounded range -> counting sort (pure O(n), no comparisons)
    if (range < th.counting_cap && range <= th.counting_load * static_cast<uint64_t>(n)) {
        try { counting(a, n, mn, range); note("counting", 0, 1); return; }
        catch (const std::bad_alloc&) { /* fall through */ }
    }
    // general integers -> radix; parallel MSD radix when configured and large.
    try {
        if (th.max_threads > 1 && n >= th.parallel_min) {
            int used = parallel_radix(a, n, th.max_threads);
            if (used > 0) { note("radix", static_cast<int>(sizeof(U)), used); return; }
        }
        radix(a, n); note("radix", static_cast<int>(sizeof(U)), 1); return;
    } catch (const std::bad_alloc&) { std::sort(a, a + n); note("std::sort", 0, 1); }
}

// map signed <-> unsigned preserving order by flipping the sign bit
template <class U>
void flip_sign_bit(U* u, size_t n) {
    const U mask = static_cast<U>(U(1) << (sizeof(U) * 8 - 1));
    for (size_t i = 0; i < n; ++i) u[i] ^= mask;
}

// move the last kk = min(k, n) elements to the front; returns kk
template <class U>
size_t take_tail(U* data, size_t n, size_t k) {
    size_t kk = k < n ? k : n;
    if (kk && kk < n) std::memmove(data, data + (n - kk), kk * sizeof(U));
    return kk;
}

// IEEE-754 <-> order-preserving unsigned key. For a float's bit pattern b:
//   positive (sign 0) -> flip the sign bit;  negative (sign 1) -> flip all bits.
// The resulting unsigned keys sort in the same order as the floats, including
// -inf < ... < -0 < +0 < ... < +inf. All-unsigned ops (no signed overflow /
// narrowing), so it stays clean under -Wconversion. bit access is via memcpy in
// the callers, since float<->uint is not a permitted alias.
inline uint32_t fkey32(uint32_t b) { return b ^ (0x80000000u | (0u - (b >> 31))); }
inline uint32_t funkey32(uint32_t k) { return k ^ (((k >> 31) - 1u) | 0x80000000u); }
inline uint64_t fkey64(uint64_t b) { return b ^ (0x8000000000000000ull | (0ull - (b >> 63))); }
inline uint64_t funkey64(uint64_t k) { return k ^ (((k >> 63) - 1ull) | 0x8000000000000000ull); }

// --- unified dispatcher support: value<->key transforms per domain -------
enum class Domain { Unsigned, Signed, Float };
inline uint32_t to_key(uint32_t v, Domain d)   { return d==Domain::Float ? fkey32(v)   : (d==Domain::Signed ? (v ^ 0x80000000u)          : v); }
inline uint64_t to_key(uint64_t v, Domain d)   { return d==Domain::Float ? fkey64(v)   : (d==Domain::Signed ? (v ^ 0x8000000000000000ull) : v); }
inline uint32_t from_key(uint32_t k, Domain d) { return d==Domain::Float ? funkey32(k) : (d==Domain::Signed ? (k ^ 0x80000000u)          : k); }
inline uint64_t from_key(uint64_t k, Domain d) { return d==Domain::Float ? funkey64(k) : (d==Domain::Signed ? (k ^ 0x8000000000000000ull) : k); }

// Engine-backed sort used whenever custom thresholds and/or stats are needed.
// Transforms to keys, (optionally) profiles the run, sorts via sluice_core with
// the given thresholds, then applies direction + first/top selection. When
// `st` is null it just sorts with the thresholds (no profiling work).
template <class T, class KeyT>
sluice_status run(T* data, size_t n, ptrdiff_t select, sluice_order order,
                  sluice_stats* st, const Thresholds& th, Domain dom) {
    if (st) { st->algorithm = "none"; st->time_ms = 0.0; st->memory_bytes = 0;
              st->passes = 0; st->already_sorted = 1; st->duplicate_pct = 0.0;
              st->range = 0.0; st->n = n; st->threads_used = 1; }
    auto apply_select = [&]() {
        if (select < 0) { size_t k = static_cast<size_t>(-select); if (k > n) k = n;
            if (k && k < n) std::memmove(data, data + (n - k), k * sizeof(T)); }
    };
    if (n < 2) return SLUICE_OK;

    std::vector<KeyT> keys;
    try { keys.resize(n); }
    catch (const std::bad_alloc&) {
        std::sort(data, data + n);
        if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
        if (st) st->algorithm = "std::sort";
        apply_select();
        return SLUICE_OK;
    }

    KeyT mn, mx; bool sorted = true;
    { KeyT k0; std::memcpy(&k0, &data[0], sizeof k0); k0 = to_key(k0, dom); mn = mx = k0; keys[0] = k0; }
    for (size_t i = 1; i < n; ++i) {
        KeyT k; std::memcpy(&k, &data[i], sizeof k); k = to_key(k, dom);
        keys[i] = k;
        if (st) { if (k < keys[i - 1]) sorted = false; if (k < mn) mn = k; else if (k > mx) mx = k; }
    }

    core_path path{ "insertion", 0, 1 };
    if (st) {
        st->already_sorted = sorted ? 1 : 0;
        st->range = static_cast<double>(mx) - static_cast<double>(mn);
        auto t0 = std::chrono::steady_clock::now();
        sluice_core(keys.data(), n, &path, th);
        auto t1 = std::chrono::steady_clock::now();
        st->time_ms   = std::chrono::duration<double, std::milli>(t1 - t0).count();
        st->algorithm = path.algorithm;
        st->passes    = path.passes;
        st->threads_used = path.threads;
        size_t distinct = 1;
        for (size_t i = 1; i < n; ++i) if (keys[i] != keys[i - 1]) ++distinct;
        st->duplicate_pct = 100.0 * (1.0 - static_cast<double>(distinct) / static_cast<double>(n));
        size_t mem = 0;
        if (std::strcmp(path.algorithm, "radix") == 0)         mem = n * sizeof(KeyT);
        else if (std::strcmp(path.algorithm, "counting") == 0) mem = (static_cast<size_t>(st->range) + 1) * sizeof(size_t);
        if (dom == Domain::Float) mem += n * sizeof(KeyT);
        st->memory_bytes = mem;
    } else {
        sluice_core(keys.data(), n, nullptr, th);
    }

    if (order == SLUICE_DESCENDING) std::reverse(keys.begin(), keys.end());
    for (size_t i = 0; i < n; ++i) { KeyT b = from_key(keys[i], dom); std::memcpy(&data[i], &b, sizeof b); }
    apply_select();
    return SLUICE_OK;
}

}  // namespace

// ------------------------------------------------------------------ C ABI
extern "C" {

// Ordered variants: sort ascending with the engine, then reverse in place for
// descending. Reversal is O(n) and in-place; for scalar integers, reversing a
// stable ascending sort is exactly the descending order.
SLUICE_API void sluice_sort_u32_ordered(uint32_t* data, size_t n, sluice_order order) {
    sluice_core(data, n);
    if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
}
SLUICE_API void sluice_sort_u64_ordered(uint64_t* data, size_t n, sluice_order order) {
    sluice_core(data, n);
    if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
}
SLUICE_API void sluice_sort_i32_ordered(int32_t* data, size_t n, sluice_order order) {
    // Accessing int32_t objects through a uint32_t lvalue is permitted: the
    // unsigned type corresponding to the dynamic type is an allowed alias
    // ([basic.lval]). Verified clean under -Wstrict-aliasing=2 and UBSan.
    uint32_t* u = reinterpret_cast<uint32_t*>(data);
    flip_sign_bit(u, n);
    sluice_core(u, n);
    flip_sign_bit(u, n);
    if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
}
SLUICE_API void sluice_sort_i64_ordered(int64_t* data, size_t n, sluice_order order) {
    uint64_t* u = reinterpret_cast<uint64_t*>(data);   // signed<->unsigned alias: OK
    flip_sign_bit(u, n);
    sluice_core(u, n);
    flip_sign_bit(u, n);
    if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
}

// Ascending shorthands (original public entry points, unchanged behaviour).
SLUICE_API void sluice_sort_u32(uint32_t* data, size_t n) { sluice_sort_u32_ordered(data, n, SLUICE_ASCENDING); }
SLUICE_API void sluice_sort_u64(uint64_t* data, size_t n) { sluice_sort_u64_ordered(data, n, SLUICE_ASCENDING); }
SLUICE_API void sluice_sort_i32(int32_t*  data, size_t n) { sluice_sort_i32_ordered(data, n, SLUICE_ASCENDING); }
SLUICE_API void sluice_sort_i64(int64_t*  data, size_t n) { sluice_sort_i64_ordered(data, n, SLUICE_ASCENDING); }

// float / double: transform each value to an order-preserving unsigned key
// (memcpy for the bit access — float<->uint is not a permitted alias), sort the
// keys with the existing engine, then transform back. NaNs are ordered by bit
// pattern, a consistent total order (unlike std::sort, for which NaN is UB).
// On allocation failure, fall back to std::sort (in-place, no extra memory).
SLUICE_API void sluice_sort_f32_ordered(float* data, size_t n, sluice_order order) {
    if (n < 2) return;
    try {
        std::vector<uint32_t> keys(n);
        for (size_t i = 0; i < n; ++i) { uint32_t b; std::memcpy(&b, &data[i], sizeof b); keys[i] = fkey32(b); }
        sluice_core(keys.data(), n);
        if (order == SLUICE_DESCENDING) std::reverse(keys.begin(), keys.end());
        for (size_t i = 0; i < n; ++i) { uint32_t b = funkey32(keys[i]); std::memcpy(&data[i], &b, sizeof b); }
    } catch (const std::bad_alloc&) {
        std::sort(data, data + n);
        if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
    }
}
SLUICE_API void sluice_sort_f64_ordered(double* data, size_t n, sluice_order order) {
    if (n < 2) return;
    try {
        std::vector<uint64_t> keys(n);
        for (size_t i = 0; i < n; ++i) { uint64_t b; std::memcpy(&b, &data[i], sizeof b); keys[i] = fkey64(b); }
        sluice_core(keys.data(), n);
        if (order == SLUICE_DESCENDING) std::reverse(keys.begin(), keys.end());
        for (size_t i = 0; i < n; ++i) { uint64_t b = funkey64(keys[i]); std::memcpy(&data[i], &b, sizeof b); }
    } catch (const std::bad_alloc&) {
        std::sort(data, data + n);
        if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
    }
}
SLUICE_API void sluice_sort_f32(float*  data, size_t n) { sluice_sort_f32_ordered(data, n, SLUICE_ASCENDING); }
SLUICE_API void sluice_sort_f64(double* data, size_t n) { sluice_sort_f64_ordered(data, n, SLUICE_ASCENDING); }

// first_n / top_n: head and tail of the array sorted in `order`.
//   first_n -> sort, keep the first k (head; already at the front)
//   top_n   -> sort, keep the last k (tail), moved to the front
// Both return the count kept (min(k, n)). With SLUICE_ASCENDING first_n is the
// k smallest and top_n the k largest; SLUICE_DESCENDING flips both.
SLUICE_API size_t sluice_first_n_u32(uint32_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_u32_ordered(data, n, order); return k < n ? k : n;
}
SLUICE_API size_t sluice_first_n_u64(uint64_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_u64_ordered(data, n, order); return k < n ? k : n;
}
SLUICE_API size_t sluice_first_n_i32(int32_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_i32_ordered(data, n, order); return k < n ? k : n;
}
SLUICE_API size_t sluice_first_n_i64(int64_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_i64_ordered(data, n, order); return k < n ? k : n;
}
SLUICE_API size_t sluice_top_n_u32(uint32_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_u32_ordered(data, n, order); return take_tail(data, n, k);
}
SLUICE_API size_t sluice_top_n_u64(uint64_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_u64_ordered(data, n, order); return take_tail(data, n, k);
}
SLUICE_API size_t sluice_top_n_i32(int32_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_i32_ordered(data, n, order); return take_tail(data, n, k);
}
SLUICE_API size_t sluice_top_n_i64(int64_t* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_i64_ordered(data, n, order); return take_tail(data, n, k);
}

// float / double first_n (head) and top_n (tail): sort in `order`, then keep
// the head or move the tail to the front. Mirrors the integer selectors.
SLUICE_API size_t sluice_first_n_f32(float* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_f32_ordered(data, n, order); return k < n ? k : n;
}
SLUICE_API size_t sluice_first_n_f64(double* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_f64_ordered(data, n, order); return k < n ? k : n;
}
SLUICE_API size_t sluice_top_n_f32(float* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_f32_ordered(data, n, order); return take_tail(data, n, k);
}
SLUICE_API size_t sluice_top_n_f64(double* data, size_t n, size_t k, sluice_order order) {
    sluice_sort_f64_ordered(data, n, order); return take_tail(data, n, k);
}

// --- unified dispatcher --------------------------------------------------
// One entry point over all six types. select > 0 keeps the first N, < 0 the top
// |N|, 0 sorts all. order NULL = ascending. collect_stats=0 takes the fast path
// (specialized functions, no profiling); collect_stats=1 fills stats. The
// specialized *_ordered / *_first_n / *_top_n functions remain the fastest
// route and are unchanged.
#define SLUICE_FAST(SUF, T)                                                     \
    do {                                                                        \
        T* p = static_cast<T*>(data);                                           \
        if (select > 0)      sluice_first_n_##SUF(p, n, static_cast<size_t>(select),  ord); \
        else if (select < 0) sluice_top_n_##SUF(p, n, static_cast<size_t>(-select), ord); \
        else                 sluice_sort_##SUF##_ordered(p, n, ord);            \
    } while (0)

SLUICE_API void sluice_config_init(sluice_config* cfg) {
    if (!cfg) return;
    Thresholds d;
    cfg->insertion_limit     = d.insertion_max;
    cfg->interpolation_limit = d.interp_max;
    cfg->interpolation_skew  = d.interp_skew;
    cfg->counting_load       = d.counting_load;
    cfg->counting_cap        = d.counting_cap;
    cfg->max_threads         = d.max_threads;   // 1 = sequential
    cfg->parallel_min        = d.parallel_min;
}

SLUICE_API sluice_status sluice_sort(sluice_dtype type, void* data, size_t n,
                                     ptrdiff_t select, const sluice_order* order,
                                     int collect_stats, sluice_stats* stats,
                                     const sluice_config* cfg) {
    const sluice_order ord = order ? *order : SLUICE_ASCENDING;
    if (data == nullptr && n > 0) return SLUICE_ERR_NULL;
    if (collect_stats && stats == nullptr) return SLUICE_ERR_NULL;

    // Fast path: default thresholds and no stats -> in-place specialized funcs.
    if (!collect_stats && cfg == nullptr) {
        switch (type) {
            case SLUICE_U32: SLUICE_FAST(u32, uint32_t); break;
            case SLUICE_I32: SLUICE_FAST(i32, int32_t);  break;
            case SLUICE_U64: SLUICE_FAST(u64, uint64_t); break;
            case SLUICE_I64: SLUICE_FAST(i64, int64_t);  break;
            case SLUICE_F32: SLUICE_FAST(f32, float);    break;
            case SLUICE_F64: SLUICE_FAST(f64, double);   break;
            default: return SLUICE_ERR_TYPE;
        }
        return SLUICE_OK;
    }

    // Custom thresholds and/or stats: build thresholds (0 = default; clamp
    // insertion/interp to the stack-scratch ceiling) and run the engine path.
    Thresholds th;
    if (cfg) {
        if (cfg->insertion_limit)     th.insertion_max = cfg->insertion_limit < INTERP_CAP ? cfg->insertion_limit : INTERP_CAP;
        if (cfg->interpolation_limit) th.interp_max    = cfg->interpolation_limit < INTERP_CAP ? cfg->interpolation_limit : INTERP_CAP;
        if (cfg->interpolation_skew)  th.interp_skew   = cfg->interpolation_skew;
        if (cfg->counting_load)       th.counting_load = cfg->counting_load;
        if (cfg->counting_cap)        th.counting_cap  = cfg->counting_cap;
        if (cfg->max_threads > 1)     th.max_threads   = cfg->max_threads;   // 0/1 = sequential
        if (cfg->parallel_min)        th.parallel_min  = cfg->parallel_min;
    }
    sluice_stats* st = collect_stats ? stats : nullptr;
    switch (type) {
        case SLUICE_U32: return run<uint32_t, uint32_t>(static_cast<uint32_t*>(data), n, select, ord, st, th, Domain::Unsigned);
        case SLUICE_I32: return run<int32_t,  uint32_t>(static_cast<int32_t*>(data),  n, select, ord, st, th, Domain::Signed);
        case SLUICE_U64: return run<uint64_t, uint64_t>(static_cast<uint64_t*>(data), n, select, ord, st, th, Domain::Unsigned);
        case SLUICE_I64: return run<int64_t,  uint64_t>(static_cast<int64_t*>(data),  n, select, ord, st, th, Domain::Signed);
        case SLUICE_F32: return run<float,    uint32_t>(static_cast<float*>(data),    n, select, ord, st, th, Domain::Float);
        case SLUICE_F64: return run<double,   uint64_t>(static_cast<double*>(data),   n, select, ord, st, th, Domain::Float);
        default: return SLUICE_ERR_TYPE;
    }
}
#undef SLUICE_FAST

SLUICE_API int sluice_is_sorted_u32(const uint32_t* data, size_t n) {
    for (size_t i = 1; i < n; ++i) if (data[i] < data[i - 1]) return 0;
    return 1;
}

SLUICE_API const char* sluice_version(void) { return "sluice 0.4.0"; }

}  // extern "C"
