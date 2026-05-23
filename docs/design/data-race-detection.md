# Design note: data-race detection (Horizon 5)

Status: **in progress — engine core only.** This note covers the design,
algorithm choice, costs, and phasing. The correctness-critical happens-before
*engine* (vector clocks + the race decision) is implemented and unit-tested in
isolation — `runtime/redzone_race.{c,h}`, exercised by
`scripts/test_race_engine.sh` in CI — but it is **not yet wired into anything**:
the per-thread clocks (TLS), synchronization interception, per-location shadow,
and pass instrumentation remain unbuilt. A normal redzone build is unaffected;
the detector is a large, separate sub-project still mostly ahead.

## Goal

Detect **data races**: two accesses to the same memory location, from different
threads, where at least one is a write, with no synchronization (no
happens-before relation) ordering them. This is the one major bug class redzone's
current address-based checking cannot find — its runtime is *thread-safe* (v0.17)
but it does not *detect races*.

The non-negotiable constraint is the project's first pillar: **no false
positives.** A race report on correctly-synchronized code would destroy trust
faster than a missed race. This single requirement drives the central design
decision below.

## Why happens-before, not lockset

There are two classic families of dynamic race detectors:

- **Lockset (Eraser)** — track the set of locks held on every access to a
  location; flag a location whose lockset becomes empty. Cheap and simple, but it
  **reports false positives** on any synchronization that isn't a lock:
  atomics, condition variables, thread create/join ordering, fork/join,
  publication via release/acquire. Real programs use all of these.
- **Happens-before (vector clocks)** — the model ThreadSanitizer uses. Track a
  logical clock per thread; a race exists only when two conflicting accesses are
  *concurrent* (neither happens-before the other) under the program's actual
  synchronization. It has **no false positives** (modulo detector bugs), at the
  cost of more state and more work per access.

Given the no-false-positives pillar, **happens-before is the only acceptable
choice.** The rest of this note assumes it.

## Architecture (reusing what redzone has)

redzone already provides three of the pieces a race detector needs:

1. **A per-access instrumentation hook.** The pass injects a call before every
   load/store today (`__redzone_check`). A race detector adds a parallel hook —
   `__redzone_race_access(addr, size, is_write)` — emitted by the same walk, and
   subject to the same selective-instrumentation skips (a provably-thread-local
   stack slot needs no race check, exactly as it needs no bounds check).
2. **A shadow-memory mechanism.** The direct-mapped shadow (one byte / 8 app
   bytes) is reused as a *pattern*, but race detection needs a **separate, richer
   shadow** (see below) — the addressability shadow stays as-is.
3. **A call-redirection mechanism.** The pass already rewrites `malloc`/`free`;
   the same machinery redirects the synchronization primitives the detector must
   observe.

### What's new

**Synchronization interception.** Happens-before is only correct if the detector
sees *every* ordering edge. The pass (or runtime interposition) must observe:

- thread lifecycle: `pthread_create`, `pthread_join`;
- mutexes: `pthread_mutex_lock`/`unlock` (and trylock, rwlocks);
- condition variables, barriers, semaphores;
- C/C++ atomics (`__atomic_*` / `atomic_*`) with their memory orders;
- thread-local handoff and `pthread_once`.

Missing any one of these is not a missed race — it is a **false positive**, which
is why this list, not the core algorithm, is the hard part.

**Race shadow.** Per memory location, store a small set of *shadow cells*, each
recording a recent access: `{ thread id, epoch (clock), is_write, size/offset }`.
TSan packs this into 4×8-byte cells per 8 application bytes (≈4× memory). On each
access the detector compares the new access against the stored cells.

**Vector clocks.** Each thread holds a vector clock (one epoch per thread). Sync
operations transfer clocks (release stores the releasing thread's clock into the
lock; acquire joins it into the acquirer's). An access at clock `C_t` races with a
stored access from thread `u` at epoch `e` iff `e > C_t[u]` (the stored access is
*not* in the current thread's past) and at least one of them is a write.

### The check, in outline

```
on access(addr, size, is_write, tid):
  cur = thread_clock[tid]            # vector clock
  for cell in race_shadow[addr]:     # recent accesses to this location
    if cell.tid != tid
       and (cell.is_write or is_write)
       and cell.epoch > cur[cell.tid]:   # concurrent, not ordered
        report_race(addr, here, cell)
  race_shadow[addr].insert({tid, cur[tid], is_write})

on mutex_unlock(m, tid):   lock_clock[m] = thread_clock[tid]
on mutex_lock(m, tid):     thread_clock[tid] |= lock_clock[m]   # join
on thread_create(child):   child_clock = parent_clock; parent_clock[parent]++
on thread_join(child):     parent_clock |= child_clock
```

## Performance & memory

This is a fundamentally heavier mode than address checking. TSan, the reference
point, runs at roughly **5–15× CPU** and **5–10× memory**. The per-access work is
no longer a single inlined shadow load+compare but a loop over shadow cells plus
vector-clock math, and the shadow is several times larger. It cannot share the
inlined fast path that makes address checking ~1.1×.

Implication: race detection should be a **distinct mode**, selected at build time
(its own pass option / runtime), not always-on alongside address checking. The
two modes can share the pass's instrumentation walk and selective-skip analysis,
but little else.

## Risks & open questions

- **Completeness of sync interception** is the correctness bottleneck (any gap →
  false positives). Atomics with relaxed/acquire/release semantics are
  particularly subtle.
- **Memory orders**: modeling C/C++ relaxed atomics precisely (they create no
  happens-before) vs. acquire/release is essential and error-prone.
- **Shadow eviction**: a fixed number of cells per location means old accesses
  are evicted; the eviction policy trades detection completeness for memory.
- **Scope**: this is plausibly large enough to be its own tool, or at least its
  own top-level mode with a separate runtime. The shared surface with the address
  checker is mostly the pass's access walk.
- **Testing**: a golden corpus of racy and race-free programs, plus differential
  testing against ThreadSanitizer, is mandatory before trusting any output.

## Phasing

1. **Happens-before engine** ✅ — the vector-clock algebra and the pairwise race
   decision, decoupled from threads and storage, unit-tested deterministically
   (`runtime/redzone_race.{c,h}`, `scripts/test_race_engine.sh`). This is the
   correctness-critical core; validating it in isolation de-risks everything
   above it.
1b. **Core happens-before MVP** (next) — drive the engine from real execution:
   per-thread clocks in TLS; intercept `pthread_mutex_lock`/`unlock` and
   `pthread_create`/`join`; a per-location shadow; instrument loads/stores via the
   pass. Detect write-write and read-write races. (Deliberately incomplete: other
   primitives aren't modeled yet, so it runs opt-in and its misses are known.)
2. **More cells + more primitives** — multiple shadow cells; rwlocks, condvars,
   barriers, semaphores, `pthread_once`.
3. **Atomics with memory orders.**
4. **Performance** — tune shadow layout and the per-access path.

## Relationship to the rest of redzone

Race detection reuses the **instrumentation pass** (the access walk and
selective-skip analysis) and the **call-redirection** approach, but uses a
**separate shadow and runtime** and a separate build mode. The existing
address/leak checker is unaffected. This separation keeps the fast, low-overhead
address checker exactly as it is, and lets the (much heavier) race mode evolve
independently.
