# Design note: inline fast-path checks + direct-mapped shadow (Horizon 4)

Status: **in progress.** Phase 1 (direct-mapped shadow) shipped in v0.10; Phase 2
(inlining the fast-path check) is next. Measured with `scripts/bench.sh`.

## Problem

redzone's per-access check is the dominant cost. For every load/store the pass
inserts:

```llvm
call void @__redzone_check(ptr %addr, i64 %size, i32 %iswrite, ptr %file, i32 %line)
```

and `__redzone_check` does a **hash-table lookup** (`app >> CHUNK_SHIFT` → probe
the chunk directory) before reading the shadow byte. So each access pays for:

1. a non-inlined **function call** (argument marshalling, call/return), and
2. a **hash lookup** (multiply, mask, probe, compare).

Measured overhead on memory-bound code is **~85–100×** (see
`docs/benchmarks.md`), versus AddressSanitizer's ~2–3×. ASan is fast because its
check is a handful of **inlined** instructions against a **direct-mapped**
shadow. We should do the same.

## Two coupled changes

### 1. Direct-mapped shadow

Replace the lazily-allocated hash of shadow chunks with a single shadow region
addressed by a fixed formula (ASan's scheme):

```
shadow_addr = (app_addr >> 3) + SHADOW_OFFSET
```

- One large `mmap` reserves the shadow up front; the OS commits pages lazily on
  first touch, so the resident cost stays proportional to memory actually used.
- Lookup collapses to **shift + add + load** — no hashing, no branching to
  allocate a chunk.
- `SHADOW_OFFSET` must be a constant shared by the runtime (which sets up the
  mapping and poisons it) and the pass (which emits the inline address math).

**The hard part (macOS / ARM64).** A single fixed-offset mapping must cover the
address ranges where heap/stack/globals actually live, without colliding with
existing mappings, under ASLR. Options, in order of preference:

1. Reserve a large region with `mmap(MAP_ANON|MAP_NORESERVE-equivalent)` and pick
   `SHADOW_OFFSET` so `(addr>>3)+offset` lands inside it for the program's actual
   address range. Probe the heap/stack base at startup to choose the offset.
2. If a single mapping proves impractical, keep a **two-level direct table**
   (top bits index a lazily-`mmap`'d second level) — still O(1), no hashing, and
   the inline math is `load(L2[top])[mid]`-style (slightly more work than a flat
   offset, but no probing).

Start with the hash→direct switch as a **runtime-internal** change (keep the
call-based check) so it can be benchmarked on its own.

### 2. Inline the fast path

Have the pass emit the common-case check inline, calling out only on a hit:

```
s = load i8 [ (addr >> 3) + OFFSET ]
if (s == 0) goto ok;                       // fully-addressable chunk: the hot path
last = (addr & 7) + size - 1;
if (s < 0 || last >= s) call __redzone_report(addr, size, iswrite, file, line);
ok:
```

- The hot path is **load + compare + branch** — no call.
- `__redzone_report` is the rare, out-of-line slow path; it reuses today's
  classification (heap table lookup for rich reports; stack/global poison codes).
- For accesses wider than 8 bytes, check the first and last byte (as today).

## Phasing (each step independently benchmarkable)

1. ✅ Runtime: hash shadow → **direct-mapped** shadow; keep `__redzone_check` a
   call (v0.10; ~15–20% win — confirms the call dominates).
2. Pass: **inline** the fast path; demote `__redzone_check` to the slow-path
   `__redzone_report`. Measure.
3. (Stretch) Skip provably-safe accesses statically; coalesce checks; this is the
   selective-instrumentation work also listed under Horizon 4.

## Correctness must not regress

The full 15-case suite and the format/integration checks must stay green at each
step; the benchmark harness additionally asserts instrumented output matches the
baseline. Target: pull memory-bound overhead from ~85–100× toward **≤ 2–3×**.

## Open questions

- The exact macOS/ARM64 `SHADOW_OFFSET` and reservation strategy (needs
  experimentation; this is the riskiest piece).
- Whether to keep the allocation table for rich reports (yes — it's only touched
  on the slow path) or move the allocation site into shadow-adjacent metadata.
- Thread-safety once the shadow is shared and written from the inline path.
