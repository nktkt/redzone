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

#ifdef __cplusplus
}
#endif

#endif // REDZONE_RACE_H
