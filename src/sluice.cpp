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
// Bucket index = off * (n-1) / range. `range` is formed by INTEGER subtraction
// (mx - mn), so it is exactly >= 1 — hence (double)range >= 1 and there is no
// floating-point division by zero, even for adjacent 64-bit magnitudes that
// collide when cast to double (the bug the integer range guards against). We
// compute scale = (n-1) / range ONCE and multiply per element — a cheap FP
// multiply, not a per-element integer division (which measurably slowed this hot
// path). The bucket is only an estimate; the insertion repair pass and skew
// guard keep the result correct regardless of float rounding.
template <class U>
bool interp_small(U* a, int n, int skew) {
    if (n < 2) return true;
    U mn = a[0], mx = a[0];
    for (int i = 1; i < n; ++i) { U x = a[i]; if (x < mn) mn = x; else if (x > mx) mx = x; }
    if (mn == mx) return true;
    const U range = static_cast<U>(mx - mn);         // exact integer diff, >= 1
    const unsigned nm1 = static_cast<unsigned>(n - 1);
    const double scale = static_cast<double>(nm1) / static_cast<double>(range);

    // Scratch stays on the stack for the default band (n <= INTERP_MAX); a config
    // that raises interpolation_max up to INTERP_CAP spills to the heap (bailing
    // to radix on allocation failure). The fast path is unchanged.
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
        int k = static_cast<int>(static_cast<double>(static_cast<U>(a[i] - mn)) * scale);
        if (k >= n) k = n - 1;                 // guard float round-up
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
int parallel_radix(U* a, size_t n, int want_threads, size_t* aux_peak = nullptr) {
    constexpr int TOPSHIFT = (static_cast<int>(sizeof(U)) - 1) * 8;
    std::vector<U> buf;
    try { buf.resize(n); } catch (const std::bad_alloc&) { return 0; }

    size_t off[257];
    size_t hist[256] = {0};
    {
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

    // Peak auxiliary heap (excluding the caller's key buffer): the partition
    // target `buf` (n elements, always live) plus the worker radix_lower buffers.
    // At most `nthreads` workers run at once, so the concurrent worker buffers
    // never exceed the sum of the `nthreads` largest buckets — a tight, data-
    // driven, scheduling-independent bound. Buckets of size 1 skip radix_lower.
    if (aux_peak) {
        size_t sizes[256];
        for (int b = 0; b < 256; ++b) sizes[b] = hist[b] > 1 ? hist[b] : 0;
        int topk = nthreads < 256 ? nthreads : 256;
        std::partial_sort(sizes, sizes + topk, sizes + 256,
                          [](size_t x, size_t y) { return x > y; });
        size_t worker_elems = 0;
        for (int i = 0; i < topk; ++i) worker_elems += sizes[i];
        *aux_peak = (n + worker_elems) * sizeof(U);
    }

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

// --- low-cardinality sort (few distinct values, any value range) --------
// When an input has only a handful of distinct values spread across a range too
// wide for counting sort, neither counting (O(range)) nor radix (full byte
// passes + ~n heap) is ideal. Tallying the distinct set directly is O(n log m)
// with m tiny and needs no heap. This catches enum/categorical data.
constexpr int LOWCARD_MAX = 16;    // treat <= this many distinct values as low-card

// Sort `a` in place given its `m` distinct values (unsorted) in `vals`
// (m <= LOWCARD_MAX). Orders the tiny distinct set, tallies each value's count,
// then emits ascending. Stack-only — no allocation, so it cannot fail.
template <class U>
void low_cardinality(U* a, size_t n, U* vals, int m) {
    insertion(vals, static_cast<size_t>(m));           // order the tiny distinct set
    size_t cnt[LOWCARD_MAX] = {0};
    for (size_t i = 0; i < n; ++i) {
        U x = a[i];
        int lo = 0, hi = m - 1, idx = 0;               // binary search in vals
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (vals[mid] == x) { idx = mid; break; }
            if (vals[mid] < x)  lo = mid + 1; else hi = mid - 1;
        }
        ++cnt[idx];
    }
    size_t o = 0;
    for (int d = 0; d < m; ++d) { U v = vals[d]; for (size_t c = cnt[d]; c > 0; --c) a[o++] = v; }
}

// --- the dispatcher -----------------------------------------------------
// Optional out-parameter: when non-null, records which path actually ran, how
// many radix passes, threads, and the auxiliary heap that path allocated (bytes,
// excluding the caller's key buffer). Left null on the fast path.
struct core_path { const char* algorithm; int passes; int threads; size_t aux_bytes; };

template <class U>
void sluice_core(U* a, size_t n, core_path* path = nullptr, const Thresholds& th = Thresholds{}) {
    auto note = [&](const char* alg, int passes, int threads, size_t aux) {
        if (path) { path->algorithm = alg; path->passes = passes; path->threads = threads; path->aux_bytes = aux; } };
    note("insertion", 0, 1, 0);
    if (n < 2) return;
    if (n < th.insertion_max) { insertion(a, n); return; }
    // small arrays: the interpolation placement sort wins here. If it detects
    // skew it returns false and we fall through to radix (n is small, so the
    // radix allocation is tiny).
    if (n <= th.interp_max && interp_small(a, static_cast<int>(n), th.interp_skew)) {
        // The interp path is stack-only in the default band; a raised interp_max
        // spills scratch to the heap for n > INTERP_MAX (key_h + cnt_h + out_h).
        size_t interp_aux = (n > INTERP_MAX)
            ? n * sizeof(uint16_t) + (n + 1) * sizeof(int) + n * sizeof(U) : 0;
        note("interpolation", 0, 1, interp_aux); return;
    }

    // one scan: min, max, and run detection in BOTH directions — cheap and
    // high-value. An array already ordered ascending returns in O(n); one
    // ordered descending is finished by a single O(n) reverse instead of a full
    // radix sort (equal integers are indistinguishable, so reversing a
    // non-increasing run yields the correct non-decreasing order).
    //
    // The scan runs in blocks so run-detection never adds a per-element branch to
    // the hot loop (which would mispredict ~50% of the time on random data). Flags
    // are updated BRANCHLESSLY (&= of a comparison). We drop each direction the
    // moment it is ruled out — checked only at block boundaries — so the common
    // cases collapse to the original scan's cost:
    //   * unsorted random: both directions die in block 1, then pure min/max
    //   * already ascending: `descending` dies in block 1, leaving a 1-comparison
    //                        ascending scan (identical to the original) over the rest
    //   * fully descending: stays in the two-flag loop, is detected, and O(n)-reversed
    constexpr size_t BLK = 256;
    U mn = a[0], mx = a[0];
    bool ascending = true, descending = true;
    size_t i = 1;
    while (i < n && descending) {              // phase 1: both directions still open
        size_t end = i + BLK < n ? i + BLK : n;
        for (; i < end; ++i) {
            U x = a[i], prev = a[i - 1];
            ascending  &= (x >= prev);
            descending &= (x <= prev);
            if (x < mn) mn = x; else if (x > mx) mx = x;
        }
    }
    // Once `descending` is ruled out (block 1 for anything not reverse-sorted) this
    // is exactly the original scan: one branchless ascending check plus min/max.
    for (; i < n; ++i) {
        U x = a[i], prev = a[i - 1];
        ascending &= (x >= prev);
        if (x < mn) mn = x; else if (x > mx) mx = x;
    }
    if (ascending)  { note("already sorted", 0, 1, 0); return; }
    if (descending) { std::reverse(a, a + n); note("reverse", 0, 1, 0); return; }

    const uint64_t range = static_cast<uint64_t>(mx) - static_cast<uint64_t>(mn);

    // bounded range -> counting sort (pure O(n), no comparisons). Fastest for
    // small ranges, so it takes precedence over the low-cardinality path.
    if (range < th.counting_cap && range <= th.counting_load * static_cast<uint64_t>(n)) {
        try { counting(a, n, mn, range);
              note("counting", 0, 1, (static_cast<size_t>(range) + 1) * sizeof(size_t)); return; }
        catch (const std::bad_alloc&) { /* fall through */ }
    }

    // wide range but few distinct values -> tally the distinct set directly.
    // Bounded probe: bails the moment a (LOWCARD_MAX+1)th distinct value appears,
    // so high-cardinality inputs (the radix case) pay only a short prefix scan.
    {
        U vals[LOWCARD_MAX]; int m = 0; bool low = true;
        for (size_t i = 0; i < n; ++i) {
            U x = a[i]; bool seen = false;
            for (int d = 0; d < m; ++d) if (vals[d] == x) { seen = true; break; }
            if (!seen) {
                if (m < LOWCARD_MAX) vals[m++] = x;
                else { low = false; break; }
            }
        }
        if (low) { low_cardinality(a, n, vals, m); note("low-cardinality", 0, 1, 0); return; }
    }
    // general integers -> radix; parallel MSD radix when configured and large.
    try {
        if (th.max_threads > 1 && n >= th.parallel_min) {
            size_t paux = 0;
            int used = parallel_radix(a, n, th.max_threads, &paux);
            if (used > 0) { note("radix", static_cast<int>(sizeof(U)), used, paux); return; }
        }
        radix(a, n); note("radix", static_cast<int>(sizeof(U)), 1, n * sizeof(U)); return;
    } catch (const std::bad_alloc&) { std::sort(a, a + n); note("std::sort", 0, 1, 0); }
}

// map signed <-> unsigned preserving order by flipping the sign bit
template <class U>
void flip_sign_bit(U* u, size_t n) {
    const U mask = static_cast<U>(U(1) << (sizeof(U) * 8 - 1));
    for (size_t i = 0; i < n; ++i) u[i] ^= mask;
}

// |select| for a negative select, computed without overflow. Negating a
// ptrdiff_t of PTRDIFF_MIN is undefined (no positive counterpart in the signed
// type), so we negate in the unsigned domain, where wraparound is well-defined
// and yields exactly the magnitude. Caller guarantees s < 0.
inline size_t neg_magnitude(ptrdiff_t s) {
    return static_cast<size_t>(0) - static_cast<size_t>(s);
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
        if (select < 0) { size_t k = neg_magnitude(select); if (k > n) k = n;
            if (k && k < n) std::memmove(data, data + (n - k), k * sizeof(T)); }
    };
    if (n < 2) return SLUICE_OK;

    // Clock spans the whole end-to-end operation (key transform, core sort,
    // direction, back-transform, selection) when profiling — not just the core.
    using clk = std::chrono::steady_clock;
    clk::time_point t0;
    if (st) t0 = clk::now();

    std::vector<KeyT> keys;
    try { keys.resize(n); }
    catch (const std::bad_alloc&) {
        // No room for the key buffer. Sort in place by the order-preserving key
        // via a comparator — correct for every domain, including float NaNs
        // (a raw std::sort over floats is UB when NaNs are present, so we must
        // not fall back to it). No auxiliary heap is used here.
        std::sort(data, data + n, [dom](const T& x, const T& y) {
            KeyT kx, ky; std::memcpy(&kx, &x, sizeof kx); std::memcpy(&ky, &y, sizeof ky);
            return to_key(kx, dom) < to_key(ky, dom);
        });
        if (order == SLUICE_DESCENDING) std::reverse(data, data + n);
        apply_select();
        if (st) { st->algorithm = "std::sort"; st->memory_bytes = 0;
                  st->time_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }
        return SLUICE_OK;
    }

    KeyT mn, mx; bool sorted = true;
    { KeyT k0; std::memcpy(&k0, &data[0], sizeof k0); k0 = to_key(k0, dom); mn = mx = k0; keys[0] = k0; }
    for (size_t i = 1; i < n; ++i) {
        KeyT k; std::memcpy(&k, &data[i], sizeof k); k = to_key(k, dom);
        keys[i] = k;
        if (st) { if (k < keys[i - 1]) sorted = false; if (k < mn) mn = k; else if (k > mx) mx = k; }
    }

    core_path path{ "insertion", 0, 1, 0 };
    sluice_core(keys.data(), n, st ? &path : nullptr, th);

    if (order == SLUICE_DESCENDING) std::reverse(keys.begin(), keys.end());
    for (size_t i = 0; i < n; ++i) { KeyT b = from_key(keys[i], dom); std::memcpy(&data[i], &b, sizeof b); }
    apply_select();

    if (st) {
        st->time_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        st->already_sorted = sorted ? 1 : 0;
        st->range        = static_cast<double>(mx) - static_cast<double>(mn);
        st->algorithm    = path.algorithm;
        st->passes       = path.passes;
        st->threads_used = path.threads;
        // duplicate_pct is profiling-only work, so it stays outside the timer.
        size_t distinct = 1;
        for (size_t i = 1; i < n; ++i) if (keys[i] != keys[i - 1]) ++distinct;
        st->duplicate_pct = 100.0 * (1.0 - static_cast<double>(distinct) / static_cast<double>(n));
        // Peak auxiliary heap. The key-transform buffer (keys[]) is ALWAYS
        // allocated once we reach here, so it is always counted; the chosen core
        // path reports its own working buffer (radix ping-pong, counting tally,
        // parallel worker buffers, interp heap spill) via path.aux_bytes.
        st->memory_bytes = n * sizeof(KeyT) + path.aux_bytes;
    }
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
        // No room for the key buffer. Sort in place by the SAME order-preserving
        // key via a comparator so NaNs keep their total order — a raw std::sort
        // over floats is undefined when NaNs are present (NaN compares false both
        // ways, violating strict-weak-ordering).
        std::sort(data, data + n, [](float x, float y) {
            uint32_t bx, by; std::memcpy(&bx, &x, sizeof bx); std::memcpy(&by, &y, sizeof by);
            return fkey32(bx) < fkey32(by);
        });
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
        // See the f32 note: sort by the order-preserving key so NaNs keep a
        // consistent total order rather than triggering std::sort's NaN UB.
        std::sort(data, data + n, [](double x, double y) {
            uint64_t bx, by; std::memcpy(&bx, &x, sizeof bx); std::memcpy(&by, &y, sizeof by);
            return fkey64(bx) < fkey64(by);
        });
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
        if (select > 0)      sluice_first_n_##SUF(p, n, static_cast<size_t>(select), ord); \
        else if (select < 0) sluice_top_n_##SUF(p, n, neg_magnitude(select), ord); \
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

// Validate a user config before it is trusted. Every field left 0 means "use
// the compiled-in default" and is always valid. A non-zero field is rejected
// only when it would produce nonsensical or unsafe dispatch: a negative skew
// (the interp guard would always bail), a negative thread count, or a resolved
// insertion_limit above the resolved interpolation_limit (which would invert the
// dispatch order and run insertion sort, O(n^2), on arrays past the interp band).
// Both limits resolve their default and the internal ceiling first, so the
// comparison matches exactly what the engine would use.
static bool config_valid(const sluice_config* cfg) {
    if (!cfg) return true;
    if (cfg->interpolation_skew < 0) return false;
    if (cfg->max_threads < 0)        return false;
    size_t ins = cfg->insertion_limit
               ? (cfg->insertion_limit     < INTERP_CAP ? cfg->insertion_limit     : INTERP_CAP)
               : INSERTION_MAX;
    size_t itp = cfg->interpolation_limit
               ? (cfg->interpolation_limit < INTERP_CAP ? cfg->interpolation_limit : INTERP_CAP)
               : INTERP_MAX;
    if (ins > itp) return false;
    return true;
}

SLUICE_API sluice_status sluice_sort(sluice_dtype type, void* data, size_t n,
                                     ptrdiff_t select, const sluice_order* order,
                                     int collect_stats, sluice_stats* stats,
                                     const sluice_config* cfg) {
    const sluice_order ord = order ? *order : SLUICE_ASCENDING;
    if (data == nullptr && n > 0) return SLUICE_ERR_NULL;
    if (collect_stats && stats == nullptr) return SLUICE_ERR_NULL;
    if (!config_valid(cfg)) return SLUICE_ERR_CONFIG;

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

SLUICE_API const char* sluice_version(void) { return "sluice 0.4.1"; }

}  // extern "C"
