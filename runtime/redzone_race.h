//===- redzone_race.h - happens-before core for data-race detection -------===//
//
// Horizon 5; see docs/design/data-race-detection.md.
//
// The vector-clock logic that decides whether two memory accesses race. It is
// deliberately decoupled from threads, synchronization interception, and shadow
// storage so the correctness-critical core can be unit-tested deterministically
// (scripts/test_race_engine.sh) with no concurrency and no flakiness.
//
// STATUS: this is step 1 of the detector -- the engine only. The surrounding
// machinery (per-thread clocks in TLS, intercepting locks/create/join/atomics,
// a per-location shadow, and pass instrumentation) is future work and NOT built
// yet, so nothing here runs in a normal redzone build.
//
//===----------------------------------------------------------------------===//
#ifndef REDZONE_RACE_H
#define REDZONE_RACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum distinct threads a vector clock tracks (a fixed bound for the MVP; a
// real build would grow this or use a sparser representation).
#define RZ_RACE_MAX_THREADS 64

typedef uint64_t rz_epoch_t;

// A vector clock: one logical epoch per thread.
typedef struct {
  rz_epoch_t t[RZ_RACE_MAX_THREADS];
} rz_vc;

void rz_vc_init(rz_vc *c);                     // set every epoch to 0
void rz_vc_tick(rz_vc *c, int tid);            // advance thread `tid`'s epoch
void rz_vc_join(rz_vc *dst, const rz_vc *src); // dst[i] = max(dst[i], src[i])
void rz_vc_copy(rz_vc *dst, const rz_vc *src);

// One recorded access to a location (a shadow cell).
typedef struct {
  int valid;        // 0 until an access has been recorded here
  int tid;          // the accessing thread
  rz_epoch_t epoch; // that thread's epoch at the time of the access
  int is_write;     // 1 = write, 0 = read
} rz_access;

// Does a new access -- by thread `tid`, with vector clock `cur`, writing iff
// `is_write` -- race with the recorded prior access `prev`? A data race requires
// all of: a valid prior access, from a DIFFERENT thread, with at least one of
// the two a write, and the two CONCURRENT -- i.e. `prev` does not happen-before
// the new access, which holds exactly when prev.epoch > cur[prev.tid].
int rz_race_check(const rz_access *prev, int tid, const rz_vc *cur, int is_write);

//===----------------------------------------------------------------------===//
// State machine: engine + per-thread clocks + per-location shadow + sync events
//===----------------------------------------------------------------------===//
//
// This composes the engine above into a working detector that operates on
// EXPLICIT thread handles and an explicit shadow -- still no OS threads, TLS, or
// pass instrumentation, so it stays unit-testable deterministically. The real
// build will drive these same operations from instrumented accesses and
// intercepted synchronization, with the per-thread clock living in TLS.

#include <stddef.h>
#include <stdint.h>

// A logical thread the detector tracks (mapped to an OS thread via TLS later).
typedef struct {
  int tid;
  rz_vc clock;
} rz_thread;

// A synchronization object's released clock (a mutex, the handoff at join, ...).
typedef struct {
  rz_vc clock;
} rz_sync;

// The shadow: a fixed open-addressing table mapping an 8-byte word to a few
// recent access cells. Bounded for the MVP; eviction may drop old accesses
// (a possible missed race -- never a false report).
#define RZ_RACE_BUCKETS (1u << 14)
#define RZ_RACE_CELLS 4

typedef struct {
  uintptr_t key; // (word address + 1); 0 means empty
  rz_access cells[RZ_RACE_CELLS];
} rz_bucket;

typedef struct {
  rz_bucket *buckets; // RZ_RACE_BUCKETS entries, owned
  int next_tid;       // next thread id to hand out
} rz_race_state;

int rz_race_state_init(rz_race_state *s);    // 0 on success, -1 on OOM
void rz_race_state_destroy(rz_race_state *s);

// Register the initial thread (e.g. main). Assigns a tid and starts its clock.
void rz_thread_init(rz_race_state *s, rz_thread *t);

// Record an access to `addr` by thread `t`; returns 1 if it races with a
// previously recorded access, else 0.
int rz_race_access(rz_race_state *s, rz_thread *t, uintptr_t addr, int is_write);

// Synchronization events that create happens-before edges.
void rz_sync_init(rz_sync *m);
void rz_mutex_release(rz_thread *t, rz_sync *m); // publish t's clock into m
void rz_mutex_acquire(rz_thread *t, rz_sync *m); // import m's clock into t
void rz_thread_create(rz_race_state *s, rz_thread *parent, rz_thread *child);
void rz_thread_join(rz_thread *parent, const rz_thread *child);

#ifdef __cplusplus
}
#endif

#endif // REDZONE_RACE_H
