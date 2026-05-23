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

`alloc_churn` stresses the allocator path the hardest; before the O(1) free
work (v0.12) it dominated the suite's wall time.

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

Current (v0.12 — O(1) allocator metadata lookup):

| benchmark    | baseline (ms) | instrumented (ms) | slowdown | correct |
|--------------|--------------:|------------------:|---------:|:-------:|
| compute      |         143.9 |             220.7 |     1.6x |   OK    |
| gather       |          22.6 |             397.4 |    17.7x |   OK    |
| alloc_churn  |           2.9 |              27.1 |     9.4x |   OK    |

These are best-case wall times; ratios wobble run-to-run (most for `alloc_churn`,
whose baseline is only a few ms — a warm allocator churns tiny blocks almost for
free — so its absolute instrumented cost, ~27 ms, is the stabler signal).

### How we got here

| benchmark | hashed (v0.9) | direct-mapped (v0.10) | inlined (v0.11) | O(1) free (v0.12) |
|---|--:|--:|--:|--:|
| compute | 13.9x | ~11x | 1.8x | **1.6x** |
| gather | 143x | ~123x | 34.5x | **17.7x** |
| alloc_churn | ~916x | ~780x | 805x | **9.4x** |

- **v0.10** replaced the hashed shadow with a direct-mapped one
  (`base + (addr>>3)`): a modest win, confirming the per-access *function call*,
  not the lookup, dominated.
- **v0.11** inlined the fast-path check (load the shadow byte, compare, branch;
  call the slow path only on a flagged byte). Compute-bound code dropped to ~1.8x
  — essentially ASan territory.
- **v0.12** made the allocator path O(1). `__redzone_free`/`realloc` previously
  located a block with an O(n) linear scan of the allocation table (which never
  shrinks, since freed blocks are quarantined), so a loop of N malloc/free pairs
  cost O(N²). Now each block stashes the index of its metadata in a header inside
  its own left red zone, so free recovers it in O(1) from the user pointer (the
  scan survives only on the error path, to classify interior/foreign pointers).
  `set_shadow_range` also became a single `memset` over the contiguous shadow
  instead of a per-chunk loop. `alloc_churn` fell **805x → ~9x** (instrumented
  wall time 2254 ms → 27 ms); `gather`, which also frees once per outer
  iteration, roughly halved (34.5x → 17.7x) as its free path stopped scanning —
  leaving it a near-pure measure of per-access overhead.

## Interpretation

The spread across benchmarks is the headline: overhead tracks **memory-access
and allocation density**, not raw CPU work.

- **`compute` (~1.6x)** is compute-bound: register-only hashing with one
  instrumented access per outer iteration. With the check inlined there is almost
  nothing to pay, so it sits in AddressSanitizer's ~2-3x range.
- **`gather` (~18x)** is the worst case: a tight dependent-gather loop where an
  inlined shadow load precedes essentially every (latency-bound) data load, so
  the extra loads serialize. Real code is rarely this pointer-chasing-heavy. Now
  that free is O(1), this is close to a pure per-access measurement.
- **`alloc_churn` (~9x)** hammers the allocator: each `malloc`/`free` goes
  through the runtime's red-zone setup/teardown and shadow poisoning. With the
  metadata lookup now O(1) (v0.12) this is bounded per-operation work rather than
  the former O(N²) blow-up; the residual is the fixed cost of red-zone
  bookkeeping over a warm allocator's few-nanosecond fast path.

The remaining per-access cost is now just the inlined shadow load + compare; the
out-of-line call to `__redzone_check` is taken only when a byte is flagged (and
re-validated there before reporting). The allocator path is O(1) per call.

The Horizon 4 shadow/check steps (direct-mapped shadow in v0.10, inlined check
in v0.11) brought compute-bound code to ~2x — ASan territory — and v0.12's O(1)
allocator metadata removed the allocator-path blow-up. The remaining headroom is
in the load-heavy `gather` case, which points at the next optional work:
**skipping provably-safe accesses** to thin out redundant checks.
