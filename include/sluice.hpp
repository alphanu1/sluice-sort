/* ==========================================================================
 * sluice.hpp — header-only C++ wrapper over the Sluice C ABI.
 *
 * Author: Alphanu1 / Ben Templaman
 * Since:  2026-07-08
 *
 * Type-safe and size-deduced: element type and length come from the container
 * (or an explicit pointer+size), and each call dispatches at COMPILE TIME to
 * the matching specialized C function — so there is no runtime type switch and
 * no overhead versus calling the C ABI directly. Unsupported element types are
 * a compile error, not a runtime check.
 *
 *     std::vector<double> v = {...};
 *     sluice::sort(v);              // ascending
 *     sluice::descending(v);
 *     sluice::first(v, 20);         // 20 smallest, at the front
 *     sluice::top(v, 20);           // 20 largest, at the front
 *
 *     sluice_stats s;
 *     sluice::sort(v, s);           // profile the run (algorithm, time, ...)
 *
 * Supported element types: uint32_t, int32_t, uint64_t, int64_t, float, double
 * (and any typedef that resolves to one of them, e.g. int, unsigned).
 * ========================================================================== */
#ifndef SLUICE_HPP
#define SLUICE_HPP

#include "sluice.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace sluice {
namespace detail {

template <class T>
struct traits {
    static_assert(sizeof(T) == 0,
        "sluice: unsupported element type — use uint32_t/int32_t/uint64_t/int64_t/float/double");
};

#define SLUICE_TRAIT(T, SUF, DT)                                                                    \
    template <> struct traits<T> {                                                                  \
        static constexpr sluice_dtype dtype = DT;                                                   \
        static void sort(T* d, std::size_t n, sluice_order o) { sluice_sort_##SUF##_ordered(d, n, o); } \
        static std::size_t first(T* d, std::size_t n, std::size_t k, sluice_order o) { return sluice_first_n_##SUF(d, n, k, o); } \
        static std::size_t top(T* d, std::size_t n, std::size_t k, sluice_order o) { return sluice_top_n_##SUF(d, n, k, o); } \
    }
SLUICE_TRAIT(std::uint32_t, u32, SLUICE_U32);
SLUICE_TRAIT(std::int32_t,  i32, SLUICE_I32);
SLUICE_TRAIT(std::uint64_t, u64, SLUICE_U64);
SLUICE_TRAIT(std::int64_t,  i64, SLUICE_I64);
SLUICE_TRAIT(float,         f32, SLUICE_F32);
SLUICE_TRAIT(double,        f64, SLUICE_F64);
#undef SLUICE_TRAIT

// Element type of a container exposing .data()/.size(); ill-formed otherwise
// (which SFINAEs the container overloads out for non-containers like pointers).
template <class C>
using elem_t = std::remove_cv_t<std::remove_pointer_t<decltype(std::declval<C&>().data())>>;

}  // namespace detail

// ---- pointer + size ----
template <class T> void sort(T* d, std::size_t n, sluice_order o = SLUICE_ASCENDING) { detail::traits<T>::sort(d, n, o); }
template <class T> void ascending(T* d, std::size_t n)  { detail::traits<T>::sort(d, n, SLUICE_ASCENDING); }
template <class T> void descending(T* d, std::size_t n) { detail::traits<T>::sort(d, n, SLUICE_DESCENDING); }
template <class T> std::size_t first(T* d, std::size_t n, std::size_t k, sluice_order o = SLUICE_ASCENDING) { return detail::traits<T>::first(d, n, k, o); }
template <class T> std::size_t top(T* d, std::size_t n, std::size_t k, sluice_order o = SLUICE_ASCENDING) { return detail::traits<T>::top(d, n, k, o); }

// ---- containers (std::vector, std::array, ...) ----
template <class C, class T = detail::elem_t<C>> void sort(C& c, sluice_order o = SLUICE_ASCENDING) { detail::traits<T>::sort(c.data(), c.size(), o); }
template <class C, class T = detail::elem_t<C>> void ascending(C& c)  { detail::traits<T>::sort(c.data(), c.size(), SLUICE_ASCENDING); }
template <class C, class T = detail::elem_t<C>> void descending(C& c) { detail::traits<T>::sort(c.data(), c.size(), SLUICE_DESCENDING); }
template <class C, class T = detail::elem_t<C>> std::size_t first(C& c, std::size_t k, sluice_order o = SLUICE_ASCENDING) { return detail::traits<T>::first(c.data(), c.size(), k, o); }
template <class C, class T = detail::elem_t<C>> std::size_t top(C& c, std::size_t k, sluice_order o = SLUICE_ASCENDING) { return detail::traits<T>::top(c.data(), c.size(), k, o); }

// ---- with profiling (routes through the unified C dispatcher) ----
template <class T>
sluice_status sort(T* d, std::size_t n, sluice_stats& stats, sluice_order o = SLUICE_ASCENDING) {
    return sluice_sort(detail::traits<T>::dtype, d, n, /*select=*/0, &o, /*collect=*/1, &stats, /*cfg=*/nullptr);
}
template <class C, class T = detail::elem_t<C>>
sluice_status sort(C& c, sluice_stats& stats, sluice_order o = SLUICE_ASCENDING) {
    return sluice_sort(detail::traits<T>::dtype, c.data(), c.size(), /*select=*/0, &o, /*collect=*/1, &stats, /*cfg=*/nullptr);
}

// ---- with custom dispatch config (and optionally stats) ----
template <class T>
sluice_status sort(T* d, std::size_t n, const sluice_config& cfg, sluice_order o = SLUICE_ASCENDING) {
    return sluice_sort(detail::traits<T>::dtype, d, n, 0, &o, 0, nullptr, &cfg);
}
template <class C, class T = detail::elem_t<C>>
sluice_status sort(C& c, const sluice_config& cfg, sluice_order o = SLUICE_ASCENDING) {
    return sluice_sort(detail::traits<T>::dtype, c.data(), c.size(), 0, &o, 0, nullptr, &cfg);
}
template <class C, class T = detail::elem_t<C>>
sluice_status sort(C& c, const sluice_config& cfg, sluice_stats& stats, sluice_order o = SLUICE_ASCENDING) {
    return sluice_sort(detail::traits<T>::dtype, c.data(), c.size(), 0, &o, 1, &stats, &cfg);
}

}  // namespace sluice

#endif  // SLUICE_HPP
