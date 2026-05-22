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
- **Command:** `./scripts/bench.sh` (min of K runs, `-O2` both)

Current (v0.11 — inlined fast-path check over a direct-mapped shadow):

| benchmark    | baseline (ms) | instrumented (ms) | slowdown | correct |
|--------------|--------------:|------------------:|---------:|:-------:|
| compute      |         144.5 |             258.2 |     1.8x |   OK    |
| gather       |          23.3 |             803.0 |    34.5x |   OK    |
| alloc_churn  |           2.8 |            2253.9 |   805.0x |   OK    |

These are best-case wall times; ratios wobble run-to-run (most for `alloc_churn`,
whose baseline is only a few ms — a warm allocator churns tiny blocks almost for
free — so its absolute instrumented cost is the stabler signal).

### How we got here

| benchmark | hashed (v0.9) | direct-mapped (v0.10) | inlined (v0.11) |
|---|--:|--:|--:|
| compute | 13.9x | ~11x | **1.8x** |
| gather | 143x | ~123x | **34.5x** |
| alloc_churn | ~916x | ~780x | **805x** |

- **v0.10** replaced the hashed shadow with a direct-mapped one
  (`base + (addr>>3)`): a modest win, confirming the per-access *function call*,
  not the lookup, dominated.
- **v0.11** inlined the fast-path check (load the shadow byte, compare, branch;
  call the slow path only on a flagged byte). Compute-bound code dropped to ~1.8x
  — essentially ASan territory — and load-bound `gather` improved ~4x.
  `alloc_churn` barely moved: its cost is the allocator path
  (`__redzone_malloc`/`free` + poisoning), not per-access checks — the next thing
  to optimize.

## Interpretation

The spread across benchmarks is the headline: overhead tracks **memory-access
and allocation density**, not raw CPU work.

- **`compute` (~1.8x)** is compute-bound: register-only hashing with one
  instrumented access per outer iteration. With the check inlined there is almost
  nothing to pay, so it sits in AddressSanitizer's ~2-3x range.
- **`gather` (~35x)** is the worst case: a tight dependent-gather loop where an
  inlined shadow load precedes essentially every (latency-bound) data load, so
  the extra loads serialize. Real code is rarely this pointer-chasing-heavy.
- **`alloc_churn` (hundreds-to-~800x)** hammers the allocator: each
  `malloc`/`free` goes through the runtime's red-zone setup/teardown and shadow
  poisoning, which dwarfs the few-nanosecond warm allocator fast path. The
  inlined access check doesn't touch this path — which is why it's the next
  optimization target.

The remaining per-access cost is now just the inlined shadow load + compare; the
out-of-line call to `__redzone_check` is taken only when a byte is flagged (and
re-validated there before reporting).

Both Horizon 4 shadow/check steps are now in place (direct-mapped shadow in
v0.10, inlined check in v0.11), bringing compute-bound code to ~2x — ASan
territory. The remaining high-overhead cases point at the next work: speeding up
the **allocator path** (`alloc_churn`) and, optionally, **skipping
provably-safe accesses** to thin out checks in load-heavy loops (`gather`).
