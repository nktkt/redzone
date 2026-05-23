//===- redzone_race.c - happens-before core (see redzone_race.h) ----------===//

#include "redzone_race.h"

#include <string.h>

void rz_vc_init(rz_vc *c) { memset(c, 0, sizeof *c); }

void rz_vc_tick(rz_vc *c, int tid) {
  if (tid >= 0 && tid < RZ_RACE_MAX_THREADS)
    c->t[tid]++;
}

void rz_vc_join(rz_vc *dst, const rz_vc *src) {
  for (int i = 0; i < RZ_RACE_MAX_THREADS; i++)
    if (src->t[i] > dst->t[i])
      dst->t[i] = src->t[i];
}

void rz_vc_copy(rz_vc *dst, const rz_vc *src) { memcpy(dst, src, sizeof *dst); }

int rz_race_check(const rz_access *prev, int tid, const rz_vc *cur,
                  int is_write) {
  if (!prev->valid)
    return 0; // nothing recorded here yet
  if (prev->tid == tid)
    return 0; // same thread: program order, never a race
  if (!prev->is_write && !is_write)
    return 0; // read/read: not a conflict
  if (prev->tid < 0 || prev->tid >= RZ_RACE_MAX_THREADS)
    return 0; // out of range: ignore defensively
  // Race iff the prior access does NOT happen-before this one, i.e. its epoch
  // is ahead of what this thread's clock knows about that thread.
  return prev->epoch > cur->t[prev->tid];
}
