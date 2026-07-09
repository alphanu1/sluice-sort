# Sluice

An adaptive integer sorting engine. A sluice channels a mixed stream and
separates it into graded outputs by routing it through the right screen — this
engine does the same: it inspects the input and **dispatches to the fastest
applicable method**, so it is never meaningfully slower than `std::sort` and
often several times faster.

```
tiny arrays (n < 16)       -> insertion sort       (no setup cost)
small arrays (n <= 512)    -> interpolation place   (Flashsort; skew-guarded)
already sorted             -> return early          (detected in the scan pass)
bounded range (<= 4n)      -> counting sort         (O(n), no comparisons)
everything else            -> LSD radix sort         (O(n·w), beats std::sort)
allocation failure         -> std::sort             (in-place safety net)
```

Non-comparison methods (interpolation, counting, radix) sidestep the
Ω(n·log n) lower bound that binds every comparison sort, which is why they win
on integer data. The interpolation path (Flashsort, Neubert 1998 — see
[References](#references)) computes each element's approximate position
directly and repairs locally; its O(n²) worst case is defused by
capping it to n <= 512 plus a skew guard that hands off to radix the moment
any bucket grows too large — so the O(n²) tail can never trigger.

## Measured vs `std::sort` (this machine, uint32)

Benchmarked over a pool of distinct arrays per size — the realistic case.
(Re-sorting one identical array instead lets the branch predictor memorise the
comparison order and flatters `std::sort`; this harness deliberately avoids
that.)

| input                    | speedup | path         |
|--------------------------|--------:|--------------|
| n=8    uniform           | ~1.0x (tie) | insertion    |
| n=50   uniform           |   2.5x  | interpolation |
| n=200  uniform           |   4.1x  | interpolation |
| n=512  uniform           |   5.1x  | interpolation |
| n=1000 uniform           |   4.9x  | radix        |
| n=100k uniform           |   7.6x  | radix        |
| n=1M   uniform           |   7.2x  | radix        |
| n=1M   bounded (<1000)   |  33.7x  | counting     |
| n=1M   already sorted    |  15.4x  | early exit   |

Reproduce with `make bench`.

### Across the full size range vs multiple methods

![Sluice speedup vs std::sort, std::stable_sort, and heapsort across n](docs/benchmark.svg)

Sweeping n from 8 to 1,048,576 (uniform random uint32), Sluice's advantage
grows from parity at the smallest sizes to 7–12× at a million elements. The
**overall geometric-mean gain is 4.7× across the field** (3.7× vs `std::sort`,
4.5× vs `std::stable_sort`, 6.4× vs `heapsort`). At n=8 the insertion path ties
`std::sort` (introsort also uses insertion for tiny inputs) while already
beating stable_sort and heapsort.

### All seven methods (absolute time per element)

![Time per element across seven sort methods, log-log](docs/methods.svg)

Every method plotted as its own curve (uniform random uint32, lower = faster).
Sluice tracks whichever underlying method is fastest at each size — insertion
below 16, interpolation to 512, radix above. Two honesty notes on the extra
lines:

- **insertion sort is O(n²)** — its per-element cost climbs linearly with n, so
  it's shown to n=16384 and then goes off-scale (it can't practically run at
  n=1M).
- **counting sort can't sort wide-range data** — its memory scales with the
  *value range*, not n (uniform uint32 would need ~8 GB of buckets). It's shown
  (dashed) on bounded input [0, n), its valid domain, where it's a flat O(n)
  line. On that domain Sluice dispatches to it, reaching 33.7× vs `std::sort`.

Note radix's high cost at tiny n (its per-call heap allocation dominates) — the
exact reason Sluice uses interpolation, not radix, below n=512.

### Interpolation vs radix in the n ≤ 512 band

The small-array band dispatches to interpolation rather than radix (the general
integer path). This is why — interpolation's gain over radix across that band,
uniform random uint32:

| n   | interpolation | radix   | interp gain |
|-----|--------------:|--------:|------------:|
| 16  |       145 ns  |  606 ns |    4.18x    |
| 32  |       270 ns  |  637 ns |    2.36x    |
| 64  |       464 ns  |  818 ns |    1.76x    |
| 128 |       871 ns  | 1215 ns |    1.40x    |
| 256 |      1655 ns  | 1967 ns |    1.19x    |
| 384 |      2474 ns  | 2732 ns |    1.10x    |
| 512 |      3279 ns  | 3516 ns |    1.07x    |

**Geomean gain over radix in this band: 1.66x.** The advantage is largest at
tiny n, where radix pays a per-call heap allocation for its ping-pong buffer
while interpolation runs entirely on the stack; it narrows toward the ~768
crossover, beyond which radix wins and the dispatcher switches to it.

## Scope & caveats

- **Integer keys only:** `uint32/int32/uint64/int64`. Radix needs fixed-width
  decomposable keys; this is the deliberate trade — specialization for speed.
  `std::sort` remains the right tool for arbitrary comparables.
- Signed types are mapped to the unsigned domain (sign-bit flip) and back.
- Uses ~n auxiliary memory (radix) or O(range) (counting); `std::sort` is
  in-place. The `std::sort` fallback covers allocation failure.
- Equal integers are indistinguishable, so results are stable by construction.
- Not novel research: this is a well-engineered combination of established
  algorithms, dispatched adaptively — the same family as `boost::spreadsort`
  and `ska_sort`. The interpolation path is **Flashsort** (Neubert, 1998); the
  bounded path is **counting sort** (Seward, 1954); the general path is **LSD
  radix sort** (Hollerith-era). See [References](#references).

## References

The named algorithms Sluice dispatches to, and their sources:

- **Interpolation path — Flashsort.** Karl-Dietrich Neubert, "The Flashsort1
  Algorithm", *Dr. Dobb's Journal* 23(2): 123–125, 131 (February 1998). An
  in-place implementation of histogram sort (a type of bucket sort) that
  classifies each element to a bucket by linear interpolation over [min, max],
  then repairs with insertion sort — exactly the interpolation path used here.
  <https://en.wikipedia.org/wiki/Flashsort>
- **Bounded path — counting sort.** Harold H. Seward, 1954.
- **General path — LSD radix sort.** Origins in Hollerith's tabulating
  machines (1880s); see D. E. Knuth, *TAOCP* Vol. 3, "Sorting and Searching".
- **Modern descendant (context).** Kristo et al., "The Case for a Learned
  Sorting Algorithm" (SIGMOD 2020) — replaces the linear interpolation with a
  learned model of the data distribution; same core idea as Flashsort.

The Sluice engine itself — the adaptive dispatcher, the skew guard, the
cross-platform build, and the benchmarks — is original work built on top of
these published methods.

## Build

Native (autodetects Linux / macOS / Windows host):

```
make            # static lib + shared lib + CLI executable
make test       # run the correctness self-test
make bench      # run the benchmark
```

Artifacts land in `build/<target>/`:

| file                     | what                          |
|--------------------------|-------------------------------|
| `libsluice.a`            | static library                |
| `libsluice.so`           | shared library (Linux)        |
| `libsluice.dylib`        | shared library (macOS)        |
| `libsluice.dll` + `.dll.a` | shared library + import lib (Windows) |
| `sluice` / `sluice.exe`  | command-line executable       |

### Cross-compiling

Pick a target and provide the matching toolchain:

```
make TARGET=windows        # needs MinGW-w64  (x86_64-w64-mingw32-g++)
make TARGET=macos          # needs osxcross   (o64-clang++)
make TARGET=linux          # needs g++/clang++
make all-targets           # builds each target you have a toolchain for
```

One-host option — `zig` bundles the libc/SDKs for every target, so a single
install cross-compiles all three:

```
make USE_ZIG=1 TARGET=windows
make USE_ZIG=1 TARGET=macos
make USE_ZIG=1 TARGET=linux
```

You can always force the compiler explicitly: `make TARGET=windows CXX=/path/to/cc`.

> Build note: the Linux artifacts in this drop were compiled and tested with
> g++ 13. The Windows and macOS targets are wired up in the Makefile and report
> the correct cross-compiler, but were not built here (no cross-toolchain in the
> build sandbox). Install MinGW-w64 / osxcross, or use `USE_ZIG=1`, to produce
> those binaries.

## Using the library

C / C++:

```c
#include "sluice.h"
uint32_t data[] = { 9, 1, 8, 2, 7 };
sluice_sort_u32(data, 5);          // -> 1 2 7 8 9
```

Link statically: `cc app.c libsluice.a -lstdc++`
Link dynamically: `cc app.c -DSLUICE_USE_SHARED -lsluice`

From Python via the shared library (no build step needed):

```python
import ctypes
lib = ctypes.CDLL("./build/linux/libsluice.so")
arr = (ctypes.c_uint32 * 5)(9, 1, 8, 2, 7)
lib.sluice_sort_u32(arr, 5)
print(list(arr))                   # [1, 2, 7, 8, 9]
```

## API

```c
void        sluice_sort_u32(uint32_t* data, size_t n);
void        sluice_sort_i32(int32_t*  data, size_t n);
void        sluice_sort_u64(uint64_t* data, size_t n);
void        sluice_sort_i64(int64_t*  data, size_t n);
int         sluice_is_sorted_u32(const uint32_t* data, size_t n);
const char* sluice_version(void);
```

## Layout

```
include/sluice.h   public C API (stable ABI, DLL export macros)
src/sluice.cpp     the engine (dispatcher + insertion/counting/radix)
src/cli.cpp        self-test + benchmark harness (the executable)
Makefile           cross-platform build
```
