// ==========================================================================
// Sluice engine implementation.
//
// One templated core operates on UNSIGNED integers. Signed entry points map
// their input to the unsigned domain with an order-preserving bijection
// (flip the sign bit), sort, then map back — so radix and counting logic stay
// uniform and branch-free.
// ==========================================================================
#include "sluice.h"

#include <algorithm>   // std::sort, std::copy
#include <cstring>     // std::memcpy
#include <new>         // std::bad_alloc
#include <vector>

namespace {

// --- tuning knobs -------------------------------------------------------
constexpr size_t   INSERTION_MAX  = 16;        // n < this: insertion sort
constexpr size_t   INTERP_MAX     = 512;       // n <= this: interpolation place
constexpr int      INTERP_SKEW    = 32;        // bail to radix if a bucket
                                               //   exceeds this (bounds repair
                                               //   work to <= 32n, defuses O(n^2))
constexpr uint64_t COUNTING_LOAD  = 4;         // counting if range <= LOAD*n
constexpr uint64_t COUNTING_CAP   = 1ull << 21;// ...and range <= ~2.1M slots

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
bool interp_small(U* a, int n) {
    if (n < 2) return true;
    U mn = a[0], mx = a[0];
    for (int i = 1; i < n; ++i) { U x = a[i]; if (x < mn) mn = x; else if (x > mx) mx = x; }
    if (mn == mx) return true;
    const U range = static_cast<U>(mx - mn);         // exact integer diff, >= 1
    const unsigned nm1 = static_cast<unsigned>(n - 1);
    uint16_t key[INTERP_MAX];
    int      cnt[INTERP_MAX + 1];
    U        out[INTERP_MAX];
    for (int i = 0; i < n; ++i) cnt[i] = 0;
    int maxbucket = 0;
    for (int i = 0; i < n; ++i) {
        int k = interp_bucket<U>(static_cast<U>(a[i] - mn), nm1, range);  // in [0, n-1]
        key[i] = static_cast<uint16_t>(k);
        int c = ++cnt[k];
        if (c > maxbucket) maxbucket = c;
    }
    if (maxbucket > INTERP_SKEW) return false; // skewed -> let radix handle it
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

// --- the dispatcher -----------------------------------------------------
template <class U>
void sluice_core(U* a, size_t n) {
    if (n < 2) return;
    if (n < INSERTION_MAX) { insertion(a, n); return; }
    // small arrays: the interpolation placement sort wins here. If it detects
    // skew it returns false and we fall through to radix (n is small, so the
    // radix allocation is tiny).
    if (n <= INTERP_MAX && interp_small(a, static_cast<int>(n))) return;

    // one scan: min, max, and "already sorted?" — cheap, high-value.
    U mn = a[0], mx = a[0];
    bool sorted = true;
    for (size_t i = 1; i < n; ++i) {
        U x = a[i];
        if (x < a[i - 1]) sorted = false;
        if (x < mn) mn = x;
        else if (x > mx) mx = x;
    }
    if (sorted) return;

    const uint64_t range = static_cast<uint64_t>(mx) - static_cast<uint64_t>(mn);

    // bounded range -> counting sort (pure O(n), no comparisons)
    if (range < COUNTING_CAP && range <= COUNTING_LOAD * static_cast<uint64_t>(n)) {
        try { counting(a, n, mn, range); return; }
        catch (const std::bad_alloc&) { /* fall through */ }
    }
    // general integers -> radix; std::sort is the in-place safety net
    try { radix(a, n); return; }
    catch (const std::bad_alloc&) { std::sort(a, a + n); }
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

SLUICE_API int sluice_is_sorted_u32(const uint32_t* data, size_t n) {
    for (size_t i = 1; i < n; ++i) if (data[i] < data[i - 1]) return 0;
    return 1;
}

SLUICE_API const char* sluice_version(void) { return "sluice 0.1.0"; }

}  // extern "C"
