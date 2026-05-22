# Design note: shadow memory (Horizon 2)

Status: **implemented (MVP)** in `runtime/redzone_rt.c` as of v0.4. The
per-access fast path is now O(1); the allocation table is consulted only on the
error path. Remaining stretch items (inlining the fast path, fixed-offset
shadow) are deferred to Horizon 4.

## Problem

Today `__redzone_check` does a linear scan of every live allocation on **every
load/store**:

```c
for (size_t i = 0; i < g_count; i++) { /* is addr in block i? */ }
```

That is `O(n)` per access, where `n` is the number of live allocations. It is
fine for the demos but collapses on real programs (millions of accesses ×
thousands of allocations). Making the per-access check `O(1)` is the central
scalability unlock of Horizon 2.

## Idea: a shadow map

Keep a second region of memory — the **shadow** — that records, for each small
chunk of application memory, whether it is addressable. A check becomes: map the
address to its shadow byte, read it, compare. One memory read instead of a scan.

### Mapping (8 bytes → 1 shadow byte)

Following AddressSanitizer's scheme, every aligned 8-byte chunk of application
memory maps to one shadow byte:

```
shadow_addr = (app_addr >> 3) + SHADOW_OFFSET
```

Shadow byte encoding:

| Value   | Meaning                                          |
|---------|--------------------------------------------------|
| `0`     | all 8 bytes addressable                          |
| `1..7`  | only the first *k* bytes addressable (partial)   |
| `0xFA`  | red zone (heap left/right) — poisoned            |
| `0xFD`  | freed memory — poisoned                          |

Partial values let us guard sizes that are not multiples of 8 (e.g. a 4-byte
`int` at the start of an 8-byte chunk → shadow `4`).

### The fast-path check

```
k = shadow[app >> 3]
if (k == 0) -> ok (whole chunk addressable)
if (k is poison) -> error
else            -> ok only if the accessed bytes fall within the first k
```

This is small enough to inline at each access site later (an optimization for
Horizon 4); for now the runtime function is fine.

## Poisoning

- `__redzone_malloc`: mark the user region addressable (shadow `0`, plus a
  partial byte for a non-8-aligned tail) and the surrounding red zones as
  `0xFA`.
- `__redzone_free`: mark the whole user region `0xFD`. Use-after-free then falls
  out of the same fast-path check — no special case needed.

## Reserving the shadow

Real ASan reserves shadow for the entire address space with one big `mmap` at a
fixed offset. That is fast but platform-specific and fiddly on macOS/ARM64
(ASLR, PIE, the high-half address layout).

**MVP plan: a two-level software page table.** A top-level array indexes the
high bits of an address; each entry lazily `mmap`s a second-level shadow chunk
the first time that region is touched. This gives `O(1)` lookup without
reserving terabytes up front, and is portable. We can switch to the fixed-offset
`mmap` scheme later (Horizon 4) for raw speed.

## Keeping rich reports

The shadow tells us *whether* an access is legal, but not the allocation size or
site. So we keep the existing allocation table — but consult it **only on the
error path**, when we are about to abort anyway. The common path stays `O(1)`;
the `O(n)` table walk happens at most once per run (when reporting the bug).

## Migration plan

1. ✅ Add the shadow map + poison/unpoison helpers; keep the table.
2. ✅ Rewrite `__redzone_check` fast path to read the shadow; on poison, fall
   back to the table for the rich report.
3. ✅ Move `__redzone_malloc`/`__redzone_free` to poison/unpoison shadow.
4. ✅ Confirm correctness on the full test corpus (10/10 pass unchanged).
5. (Later, Horizon 4) inline the fast path in the pass; fixed-offset shadow;
   add benchmarks.

## What shipped

The MVP uses a **lazily-allocated hash of fixed-size shadow chunks** (each
covering 64 KiB of app memory, 8 KiB of shadow) rather than a two-level radix
table — same lazy, portable, O(1) properties, simpler code. Encoding is signed:
`0` fully addressable, `1..7` partial tail, negative poisoned (`0xFA` red zone,
`0xFD` freed). `__redzone_check` tests the first and last byte of each access.

## Open questions

- Shadow granularity: stick with 8:1, or make it configurable?
- macOS/ARM64: confirm the address ranges we must cover for heap allocations.
- Thread safety: the table is currently lock-free and single-threaded; shadow
  writes from `malloc`/`free` will need the same care when we add threading.
