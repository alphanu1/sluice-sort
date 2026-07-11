/* ==========================================================================
 * Sluice — an adaptive numeric sorting engine that routes every dataset
 * through its fastest available sorting strategy.
 *
 * Author: Alphanu1 / Ben Templaman
 * Since:  2026-07-08
 *
 * A "sluice" channels a mixed stream and separates it into graded outputs by
 * routing it through the right screen. This engine does the same: it inspects
 * the input and dispatches to the fastest applicable method —
 *
 *     tiny arrays (n<16)    -> insertion sort      (no setup cost)
 *     small arrays (n<=512) -> interpolation place  (branch-light; ~2-5x; skew-guarded)
 *     already sorted        -> return early         (detected in the scan pass)
 *     bounded range         -> counting sort        (O(n), no comparisons)
 *     everything else       -> LSD radix sort        (O(n·w), beats std::sort)
 *     allocation fails      -> std::sort             (in-place safety net)
 *
 * so it is never meaningfully slower than std::sort and often several times
 * faster. Non-comparison methods sidestep the Ω(n·log n) comparison bound.
 *
 * This header exposes a stable C ABI so the shared library is callable from
 * C, C++, Python (ctypes/cffi), Rust, Go, etc.
 * ==========================================================================*/
#ifndef SLUICE_H
#define SLUICE_H

#include <stddef.h>
#include <stdint.h>

/* --- export/import visibility ------------------------------------------
 * Define SLUICE_BUILD_SHARED when compiling the shared library.
 * Define SLUICE_USE_SHARED   when consuming the shared library on Windows.
 * Define neither for static-library or single-TU use. */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(SLUICE_BUILD_SHARED)
#    define SLUICE_API __declspec(dllexport)
#  elif defined(SLUICE_USE_SHARED)
#    define SLUICE_API __declspec(dllimport)
#  else
#    define SLUICE_API
#  endif
#else
#  if defined(SLUICE_BUILD_SHARED)
#    define SLUICE_API __attribute__((visibility("default")))
#  else
#    define SLUICE_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Sort direction. Ascending is the default throughout. */
typedef enum {
    SLUICE_ASCENDING  = 0,
    SLUICE_DESCENDING = 1
} sluice_order;

/* Sort an array in place, ascending. n may be 0. Data must not be NULL when
 * n > 0. Equal integer values are indistinguishable, so the result is stable
 * by construction. Shorthand for the *_ordered forms with SLUICE_ASCENDING. */
SLUICE_API void sluice_sort_u32(uint32_t* data, size_t n);
SLUICE_API void sluice_sort_i32(int32_t*  data, size_t n);
SLUICE_API void sluice_sort_u64(uint64_t* data, size_t n);
SLUICE_API void sluice_sort_i64(int64_t*  data, size_t n);

/* Sort in place in the requested direction (SLUICE_ASCENDING/SLUICE_DESCENDING). */
SLUICE_API void sluice_sort_u32_ordered(uint32_t* data, size_t n, sluice_order order);
SLUICE_API void sluice_sort_i32_ordered(int32_t*  data, size_t n, sluice_order order);
SLUICE_API void sluice_sort_u64_ordered(uint64_t* data, size_t n, sluice_order order);
SLUICE_API void sluice_sort_i64_ordered(int64_t*  data, size_t n, sluice_order order);

/* Floating point: sorted via an IEEE-754 order-preserving key transform, so the
 * usual engine paths apply. Ordering is -inf < ... < -0 < +0 < ... < +inf; NaNs
 * are ordered by bit pattern (a consistent total order — well-defined, unlike
 * std::sort where NaN breaks the ordering). Ascending is the default. */
SLUICE_API void sluice_sort_f32(float*  data, size_t n);
SLUICE_API void sluice_sort_f64(double* data, size_t n);
SLUICE_API void sluice_sort_f32_ordered(float*  data, size_t n, sluice_order order);
SLUICE_API void sluice_sort_f64_ordered(double* data, size_t n, sluice_order order);

/* Head and tail of the array sorted in `order`. first_n keeps the first k
 * (the head); top_n keeps the last k (the tail). Both sort in place, move the
 * kept run to the FRONT of data, and return the count kept (min(k, n)). With
 * SLUICE_ASCENDING (the default) first_n gives the k smallest and top_n the k
 * largest; SLUICE_DESCENDING flips both. */
SLUICE_API size_t sluice_first_n_u32(uint32_t* data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_first_n_i32(int32_t*  data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_first_n_u64(uint64_t* data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_first_n_i64(int64_t*  data, size_t n, size_t k, sluice_order order);

SLUICE_API size_t sluice_top_n_u32(uint32_t* data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_top_n_i32(int32_t*  data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_top_n_u64(uint64_t* data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_top_n_i64(int64_t*  data, size_t n, size_t k, sluice_order order);

SLUICE_API size_t sluice_first_n_f32(float*  data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_first_n_f64(double* data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_top_n_f32(float*  data, size_t n, size_t k, sluice_order order);
SLUICE_API size_t sluice_top_n_f64(double* data, size_t n, size_t k, sluice_order order);

/* ----- unified dispatcher (one call over every type) --------------------
 * The specialized functions above remain the fastest route. This convenience
 * entry point selects among them at runtime by element type, and can also
 * profile the run. */

/* Supported element types. Passing anything else returns SLUICE_ERR_TYPE. */
typedef enum {
    SLUICE_U32 = 0, SLUICE_I32, SLUICE_U64, SLUICE_I64, SLUICE_F32, SLUICE_F64
} sluice_dtype;

/* Return status. */
typedef enum {
    SLUICE_OK       =  0,
    SLUICE_ERR_TYPE = -1,   /* unsupported dtype */
    SLUICE_ERR_NULL = -2    /* NULL data with n>0, or stats requested but NULL */
} sluice_status;

/* Profiling info, filled when collect_stats is nonzero. */
typedef struct {
    const char* algorithm;      /* "insertion"|"interpolation"|"counting"|"radix"|"already sorted"|"std::sort" */
    double      time_ms;        /* wall time of the sort */
    size_t      memory_bytes;   /* auxiliary heap the chosen path used */
    int         passes;         /* radix passes (0 for other paths) */
    int         already_sorted; /* 1 if the input was already in order */
    double      duplicate_pct;  /* 100 * (1 - distinct/n) */
    double      range;          /* span of the sorted key domain (= value span for integers) */
    size_t      n;              /* element count */
    int         threads_used;   /* worker threads used (1 = sequential) */
} sluice_stats;

/* Dispatch tuning. Optimal thresholds vary by CPU; override any you like.
 * A field left 0 uses the compiled-in default, so `sluice_config c = {0};
 * c.interpolation_limit = 768;` changes only that one knob. Or call
 * sluice_config_init(&c) to fill every field with its default, then tweak.
 * interpolation_limit is clamped to an internal ceiling (4096). */
typedef struct {
    size_t   insertion_limit;      /* n < this -> insertion sort   (default 16)      */
    size_t   interpolation_limit;  /* n <= this -> interpolation   (default 512)     */
    int      interpolation_skew;   /* interp bucket-skew bail      (default 32)      */
    uint64_t counting_load;        /* counting if range <= load*n  (default 4)       */
    uint64_t counting_cap;         /* ...and range < cap           (default 2097152) */
    int      max_threads;          /* parallel radix on large n; 0/1 = sequential    */
    size_t   parallel_min;         /* only parallelize when n >= this (default 262144)*/
} sluice_config;

/* Fill cfg with the default thresholds. */
SLUICE_API void sluice_config_init(sluice_config* cfg);

/* Unified sort. `type` selects the element type; `select` > 0 keeps the first N
 * (smallest/head), < 0 keeps the top |N| (largest/tail), 0 sorts everything;
 * `order` may be NULL for ascending. When `collect_stats` is nonzero, `stats`
 * (must be non-NULL) is filled with profiling info at some cost. `cfg` may be
 * NULL for default thresholds; when non-NULL, custom tuning is applied (this
 * routes through the general engine rather than the in-place specialized path).
 * Returns SLUICE_OK, SLUICE_ERR_TYPE, or SLUICE_ERR_NULL. */
SLUICE_API sluice_status sluice_sort(sluice_dtype type, void* data, size_t n,
                                     ptrdiff_t select, const sluice_order* order,
                                     int collect_stats, sluice_stats* stats,
                                     const sluice_config* cfg);

/* Returns 1 if the array is already in non-decreasing order, else 0.
 * Provided because "is it already sorted?" is a cheap, common query. */
SLUICE_API int  sluice_is_sorted_u32(const uint32_t* data, size_t n);

/* Library version, e.g. "sluice 0.4.1". */
SLUICE_API const char* sluice_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SLUICE_H */
