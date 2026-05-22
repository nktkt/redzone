# redzone performance benchmarks

This document describes how redzone's **runtime overhead** is measured and
records a baseline result set to improve against. Overhead is the slowdown of an
**instrumented** build (compiled with the redzone pass + linked against the
runtime) relative to an **uninstrumented** baseline of the same program.

The harness lives at [`scripts/bench.sh`](../scripts/bench.sh); the benchmark
programs are in [`bench/`](../bench).

## How to run

```sh
./scripts/bench.sh        # min of 5 runs per binary (default)
./scripts/bench.sh 9      # min of 9 runs
BENCH_RUNS=3 ./scripts/bench.sh
```

The script builds the pass plugin if `build/libRedzonePass.so` is missing,
compiles the runtime once, builds every `bench/*.c` two ways, runs the
correctness check, and prints the results table. All build/run artifacts go to a
`mktemp -d` directory that is removed on exit, so nothing is left in the repo.

The instrumented `alloc_churn` run is intentionally slow (it stresses the
allocator path the hardest), so the full suite takes roughly a minute.

## Methodology

The numbers are only meaningful if the optimizer does the **same real work** in
both builds. The trap at `-O2` is that the optimizer can delete a benchmark's
work in the *baseline* (dead-store elimination, allocation removal, constant
folding) but **cannot** delete it in the instrumented build, because the
inserted `__redzone_check` calls are opaque and keep the surrounding loads/stores
live. That asymmetry would produce a meaningless, wildly inflated ratio. To
avoid it, every benchmark and the harness follow these rules:

- **`-O2` for both builds.** An `-O0` baseline is unrealistically slow and would
  inflate the ratio. The pass is verified to work correctly at `-O2`.
- **Optimization barriers in every benchmark, applied equally to both builds:**
  - the iteration count is read from `argv`, so it is unknown at compile time;
  - the final result is written to a `volatile` global sink and printed;
  - the hot loop uses **data-dependent** memory access the optimizer cannot fold
    — a gather `sum += a[a[i]]`, where `a[i]` is the address of the second load,
    forcing a real dependent load.
- **Every benchmark is bug-free** (all accesses in-bounds, everything freed) so
  the instrumented build runs to completion — a real bug would abort it and there
  would be nothing to time.
- **Min of K runs.** Each binary is run K times (default 5) and the **minimum**
  wall-clock time is reported. The minimum is the most stable estimator of
  best-case throughput: it discards the cold-start first run (process startup,
  cold caches, lazy page faults) and scheduler-noise outliers.
- **Timing** uses a Python `time.perf_counter()` monotonic clock around a
  `subprocess.run` of each binary, with a fixed workload argument so baseline and
  instrumented runs do identical work.
- **Correctness check.** For each benchmark the harness asserts that the
  instrumented binary's stdout equals the baseline's stdout. A mismatch (or an
  abort from a spurious redzone error) marks the benchmark `FAIL` and makes the
  script exit nonzero — this catches instrumentation bugs, not just slowness.

### The benchmarks

| program | what it stresses | memory-access density |
|---|---|---|
| [`bench/gather.c`](../bench/gather.c) | data-dependent gather over a fresh array each outer iteration | high (load-bound) |
| [`bench/alloc_churn.c`](../bench/alloc_churn.c) | many small `malloc`/`free` pairs, each block lightly touched | high allocator traffic |
| [`bench/compute.c`](../bench/compute.c) | register-only integer/hash mixing, one array touch per outer iteration | very low |

## Results

Measured on this machine:

- **Hardware:** Apple M5 (Mac17,2)
- **OS:** macOS 26.3.1
- **Compiler:** Homebrew clang 22.1.4 (LLVM 22)
- **Command:** `./scripts/bench.sh` (min of 5 runs, `-O2` both)

| benchmark    | baseline (ms) | instrumented (ms) | slowdown | correct |
|--------------|--------------:|------------------:|---------:|:-------:|
| compute      |         142.1 |            1970.0 |    13.9x |   OK    |
| gather       |          22.3 |            3188.8 |   143.0x |   OK    |
| alloc_churn  |           2.5 |            2290.9 |   916.4x |   OK    |

These are best-case wall times; run-to-run the ratios wobble (a few percent for
`compute`/`gather`, more for `alloc_churn`). The `alloc_churn` baseline is only a
few milliseconds — warm, glibc/libmalloc churns tiny blocks almost for free — so
its slowdown ratio is the least precise of the three (expect it in the high
hundreds to ~1000x); the absolute instrumented cost (~2.3 s for 100k
alloc/free pairs) is the stable signal there.

## Interpretation

The spread across benchmarks is the headline: overhead tracks **memory-access
and allocation density**, not raw CPU work.

- **`compute` (~14x)** is compute-bound: the inner loop is register-only hashing
  with a single instrumented array access per outer iteration. There is almost
  nothing to check, so overhead is comparatively small.
- **`gather` (~140x)** is load-bound: a `__redzone_check` precedes essentially
  every load in the hot loop.
- **`alloc_churn` (hundreds-to-~1000x)** hammers the allocator: each
  `malloc`/`free` goes through the runtime's red-zone setup/teardown and shadow
  bookkeeping, which dwarfs the few-nanosecond warm allocator fast path it
  replaces.

The reason for the high end is the current implementation strategy: each checked
access is an **out-of-line function call** to `__redzone_check`, which performs a
**hash-table shadow lookup**. A call + hash probe per memory access is inherently
expensive, so memory-bound code pays dearly while compute-bound code barely
notices.

**These numbers are the baseline to improve against, not a final target.**
Horizon 4 on the [roadmap](../ROADMAP.md) targets inlining the fast-path check
(no call on the common in-bounds case) and replacing the hash table with a
**direct-mapped shadow**, the approach AddressSanitizer uses to reach roughly
**2-3x**. The expectation is that those two changes pull the memory-bound cases
(`gather`, `alloc_churn`) down by one to two orders of magnitude; this table is
the before-picture for that work.
