/* ==========================================================================
 * Sluice — an adaptive integer sorting engine.
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

/* Sort an array in place, ascending. n may be 0. Data must not be NULL when
 * n > 0. Equal integer values are indistinguishable, so the result is stable
 * by construction. */
SLUICE_API void sluice_sort_u32(uint32_t* data, size_t n);
SLUICE_API void sluice_sort_i32(int32_t*  data, size_t n);
SLUICE_API void sluice_sort_u64(uint64_t* data, size_t n);
SLUICE_API void sluice_sort_i64(int64_t*  data, size_t n);

/* Returns 1 if the array is already in non-decreasing order, else 0.
 * Provided because "is it already sorted?" is a cheap, common query. */
SLUICE_API int  sluice_is_sorted_u32(const uint32_t* data, size_t n);

/* Library version, e.g. "sluice 0.1.0". */
SLUICE_API const char* sluice_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SLUICE_H */
