# Design note: data-race detection (Horizon 5)

Status: **in progress — engine + deterministic state machine.** This note
covers the design, algorithm choice, costs, and phasing. The
correctness-critical happens-before *engine* (vector clocks + the race decision)
**and** a full **state machine** built on it — explicit per-thread clocks, a
per-location shadow, and mutex/create/join synchronization events — are
implemented and unit-tested in isolation (`runtime/redzone_race.{c,h}`, exercised
by `scripts/test_race_engine.sh` in CI). A **runtime layer**
(`runtime/redzone_race_rt.{c,h}`) drives that state machine from *real* pthreads
— per-thread clocks in TLS, `pthread_create`/`join` wrappers, and mutex
acquire/release — validated with actual threads (`scripts/test_race_runtime.sh`).
And the **pass now emits the hooks automatically** in race mode
(`-redzone-race`): before every load/store it inserts a race-access call and it
redirects the pthread synchronization primitives to the runtime wrappers, so an
ordinary program is instrumented end to end with no manual calls
(`scripts/test_race_e2e.sh` flags a racy program and clears a mutex-protected
one). A normal (address-mode) redzone build is completely unaffected. What
remains is breadth: more synchronization primitives, C/C++ atomics with memory
orders, and performance — the detector is still an opt-in, deliberately
incomplete mode.

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
1a. **Deterministic state machine** ✅ — compose the engine into a working
   detector over *explicit* thread handles, an open-addressing per-location shadow
   (`RZ_RACE_BUCKETS` × `RZ_RACE_CELLS`), and synchronization events
   (`rz_mutex_acquire`/`release`, `rz_thread_create`/`join`). Still no OS threads,
   TLS, or pass — every operation is called by hand, so the whole detector logic
   (clock transfer, shadow eviction, the per-access check loop) is unit-tested
   with zero concurrency (`tests/race_engine_test.c`, scenarios A–I). Eviction and
   a full shadow table can only drop old accesses → missed races, never false
   reports.
1b. **Core happens-before MVP** ✅ — the state machine now runs end to end on a
   real build. The runtime (`runtime/redzone_race_rt.{c,h}`,
   `runtime/redzone_race_main.c`) is a process-global detector serialized by one
   lock: per-thread clocks in TLS, `pthread_create`/`join` wrappers that build the
   parent↔child edges, mutex acquire/release keyed by lock address, and a shadow
   attached to live addresses. The pass (`-redzone-race`) emits the access hooks
   before every load/store and redirects the pthread primitives to those
   wrappers, so an ordinary program is instrumented automatically. Validated two
   ways: the runtime alone with **real pthreads**
   (`scripts/test_race_runtime.sh`, 25× — mutex-protected → 0, create/join handoff
   → 0, unsynchronized → ≥1) and **end to end** through the pass
   (`scripts/test_race_e2e.sh` — a racy program is flagged, correctly
   synchronized ones run clean, all run repeatedly since happens-before is
   timing-independent). It detects write-write and read-write races. (Runs opt-in;
   its misses are known.)
2. **More primitives** (in progress) — modeled so far: `pthread_create`/`join`,
   `pthread_mutex_lock`/`unlock`/`trylock`, reader/writer locks
   (`pthread_rwlock_rdlock`/`wrlock`/`tryrdlock`/`trywrlock`/`unlock`), and
   condition variables (`pthread_cond_wait`/`timedwait`). An rwlock is treated as
   an acquire/release on one sync object, which over-approximates ordering between
   concurrent readers (harmless — read/read never races) while capturing every
   real reader↔writer edge, so it stays free of false positives. A condvar wait is
   modeled as the mutex unlock/re-lock it performs internally (the data it guards
   is published through that mutex), so `cond_signal`/`broadcast` need no edge.
   Still ahead: barriers, semaphores, and `pthread_once` (note: barriers and
   unnamed POSIX semaphores aren't available on macOS, the CI platform). Each
   primitive has a case in `scripts/test_race_e2e.sh`. Also: multiple shadow cells
   per location.
3. **Atomics** (initial support done) — atomic loads/stores, `atomicrmw`, and
   `cmpxchg` are modeled as acquire/release on a per-location sync object (an
   atomic read acquires after the op, a write releases before it, an RMW does
   both), so correct lock-free code no longer false-positives
   (`scripts/test_race_e2e.sh` covers a release/acquire publication and an atomic
   counter). The release-before / acquire-after placement makes the happens-before
   real (`release` is recorded before the store, the observing `acquire` after the
   load). Memory orders are *over-approximated*: a `relaxed` op is treated as
   ordering, which can only miss a race, never invent one. Still ahead: precise
   per-order modeling and standalone fences (`atomic_thread_fence`), which are
   currently ignored.
4. **Performance** — tune shadow layout and the per-access path.

## Relationship to the rest of redzone

Race detection reuses the **instrumentation pass** (the access walk and
selective-skip analysis) and the **call-redirection** approach, but uses a
**separate shadow and runtime** and a separate build mode. The existing
address/leak checker is unaffected. This separation keeps the fast, low-overhead
address checker exactly as it is, and lets the (much heavier) race mode evolve
independently.
