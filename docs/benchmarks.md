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

Current (v0.13 — selective instrumentation: skip provably-safe accesses):

| benchmark    | baseline (ms) | instrumented (ms) | slowdown | correct |
|--------------|--------------:|------------------:|---------:|:-------:|
| compute      |         143.7 |             157.5 |     1.1x |   OK    |
| gather       |          22.5 |             214.3 |     9.5x |   OK    |
| alloc_churn  |           2.8 |              20.9 |     7.5x |   OK    |

These are best-case wall times; ratios wobble run-to-run (most for `alloc_churn`,
whose baseline is only a few ms — a warm allocator churns tiny blocks almost for
free — so its absolute instrumented cost, ~21 ms, is the stabler signal).

### How we got here

| benchmark | hashed (v0.9) | direct-mapped (v0.10) | inlined (v0.11) | O(1) free (v0.12) | selective (v0.13) |
|---|--:|--:|--:|--:|--:|
| compute | 13.9x | ~11x | 1.8x | 1.6x | **1.1x** |
| gather | 143x | ~123x | 34.5x | 17.7x | **9.5x** |
| alloc_churn | ~916x | ~780x | 805x | 9.4x | **7.5x** |

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
- **v0.13** added **selective instrumentation** (see
  [`docs/design/selective-instrumentation.md`](design/selective-instrumentation.md)):
  the pass now skips checks it can prove are safe — accesses provably in-bounds of
  a static `alloca`, and redundant rechecks of a pointer already checked earlier
  in the same basic block. Across the corpus this drops 80–90% of checks (e.g.
  `compute` 62→5, `gather`/`alloc_churn` 47→5) with no loss of detection. The big
  runtime win is indirect: an unchecked scalar local is no longer pinned in memory
  by a check on its address, so `mem2reg` promotes it to a register. `compute`
  fell to **~1.1x** (essentially free) and `gather` halved again (17.7x → **9.5x**)
  as its loop induction/accumulator variables promoted; only its two genuine
  data-dependent heap loads per iteration remain checked.

## Interpretation

The spread across benchmarks is the headline: overhead tracks **memory-access
and allocation density**, not raw CPU work.

- **`compute` (~1.1x)** is compute-bound: register-only hashing with one
  instrumented access per outer iteration. Selective instrumentation (v0.13) skips
  the checks on its scalar locals, so they promote to registers and the only cost
  left is the single real array touch — barely measurable.
- **`gather` (~9.5x)** is the worst case: a tight dependent-gather loop where an
  inlined shadow load precedes essentially every (latency-bound) data load, so
  the extra loads serialize. Real code is rarely this pointer-chasing-heavy. After
  v0.13 only the two genuine data-dependent heap loads per iteration are checked,
  which is close to the irreducible per-access floor for this access pattern.
- **`alloc_churn` (~7.5x)** hammers the allocator: each `malloc`/`free` goes
  through the runtime's red-zone setup/teardown and shadow poisoning. With the
  metadata lookup O(1) (v0.12) this is bounded per-operation work rather than the
  former O(N²) blow-up; the residual is the fixed cost of red-zone bookkeeping
  over a warm allocator's few-nanosecond fast path.

The remaining per-access cost is now just the inlined shadow load + compare on the
accesses we cannot prove safe; the out-of-line call to `__redzone_check` is taken
only when a byte is flagged (and re-validated there before reporting). The
allocator path is O(1) per call.

Across v0.10–v0.13 the four Horizon 4 levers — direct-mapped shadow, inlined
check, O(1) allocator metadata, and selective instrumentation — brought
compute-bound code to ~1.1x and roughly halved the load- and allocation-bound
cases twice over. The remaining `gather` overhead is the cost of checking
genuinely unprovable pointer-chasing loads; thinning it further would require
range/loop analysis to hoist or coalesce checks (a possible future step).
