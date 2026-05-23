# Design note: selective instrumentation (Horizon 4)

Status: **done** (v0.13). Two sound, false-negative-free skips were added to the
pass; see `docs/benchmarks.md` for the resulting numbers.

## Problem

The pass inserts a check before *every* `load`/`store`. Most of those accesses
can never fault: a write to a scalar local, a fixed-index read of a small array,
or a second access to a pointer that was just checked. Instrumenting them costs
three ways:

1. **Runtime** — an extra inlined shadow load + compare per access.
2. **Lost optimization** — the inline check does `ptrtoint` on the pointer, which
   *takes the address* of the underlying object. That blocks `mem2reg`/SROA from
   promoting a scalar local to a register, so an otherwise-free variable is
   forced to live in memory and pay for a real load/store on every use. This is
   the dominant cost on compute-bound code.
3. **Code size / compile time** — more IR to optimize and more machine code.

Production AddressSanitizer skips such accesses (`isSafeAccess`,
`-asan-opt-same-temp`). redzone now does too. The cardinal rule: **never skip a
check unless the access is provably safe** — a memory-safety tool's value is its
absence of false negatives.

## The pass runs early

redzone instruments at the *start* of the pipeline (`PipelineStartEPCallback`),
i.e. on pre-optimization IR. Every scalar local is still an `alloca` and every
fixed-index access is a constant-offset GEP at that point, so these skips apply
broadly — and because we then *don't* take those addresses, the rest of the `-O2`
pipeline can clean up exactly as it would for an uninstrumented build.

## Two skips

### 1. Provably in-bounds of a static alloca

For each access, strip constant offsets from the pointer
(`stripAndAccumulateConstantOffsets`). If the base is a **static `alloca`** of
known size `S` and the access `[offset, offset+size)` lies within `[0, S)`, the
access cannot reach a red zone — skip it. (Same arithmetic as ASan's
`isSafeAccess`.)

Two constraints make this safe:

- **Allocas only**, not heap or globals. Heap is where most bugs live, and a
  global's size analysis would interact with the global red-zone wrapping. Keeping
  the skip to allocas means we never reason about a red-zoned object.
- **Evaluated before alloca wrapping.** The pass enlarges each static alloca with
  red zones and re-points uses into the padded allocation. *After* that, an
  out-of-bounds access would sit inside the bigger allocation and look in-bounds —
  a false negative. So the skip set is computed on the original pointers first
  (keyed by the `Instruction*`, which survives the later RAUW), and the wrapping
  runs afterward.

This is what catches `examples/stack_inbounds_then_oob.c`: the in-bounds writes
`a[0..3]` are skipped, but the constant `a[4]` is *not* in-bounds of `int[4]`, so
it stays checked and the overflow is still reported.

### 2. Redundant rechecks within a basic block

Walk each basic block in order, tracking the largest size each pointer has been
checked at. A later `load`/`store` of the **same pointer value** with a `<=` size
needs no recheck: the earlier check already aborted if the address was poisoned,
and **only a call** (`malloc`/`free`/`realloc`, or anything that might free) can
change an address's shadow between the two. So the map is cleared at every call
and at basic-block boundaries; anything surviving is genuinely redundant.

Pointer *identity* (same SSA value) guarantees the same runtime address, so this
needs no aliasing analysis. It mainly removes the second half of read-modify-write
pairs (`a[i] = a[i] + 1`).

## Why it doesn't introduce false negatives

- Skip (1) only drops accesses statically proven inside a non-red-zoned object.
- Skip (2) only drops an access whose address was already validated earlier in
  straight-line code with nothing that could repoison it; if the address is bad,
  the *kept* earlier check fires first and aborts before the skipped access runs.

The full example suite (now including a dedicated in-bounds-then-overflow case)
and the benchmark correctness checks stay green, confirming detection is intact.

## Results

Across the corpus the skips remove ~80–90% of checks (e.g. `compute` 62→5,
`gather`/`alloc_churn` 47→5). Runtime: `compute` ~1.6x → **~1.1x** (scalar locals
promote to registers), `gather` 17.7x → **9.5x**, `alloc_churn` 9.4x → **7.5x**.
The pass prints the skip breakdown, e.g.:

```
[redzone] instrumented 5 access(es) (skipped 56 safe + 1 redundant); ...
```

## Possible future steps

- **Cross-block redundancy** via dominator info (skip a check dominated by an
  equal/larger check with no call on the path).
- **Loop/range analysis** to hoist or coalesce checks for affine accesses — the
  only thing that would meaningfully cut the residual `gather` overhead, whose
  remaining checks are genuine, unprovable data-dependent loads.
- **Per-function/per-file opt-out attributes** for hand-audited hot code.
