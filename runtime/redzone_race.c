//===- redzone_race.c - happens-before core (see redzone_race.h) ----------===//

#include "redzone_race.h"

#include <stdlib.h>
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

//===----------------------------------------------------------------------===//
// State machine: engine + per-thread clocks + per-location shadow + sync events
//===----------------------------------------------------------------------===//

int rz_race_state_init(rz_race_state *s) {
  s->buckets = calloc(RZ_RACE_BUCKETS, sizeof *s->buckets);
  if (!s->buckets)
    return -1;
  s->next_tid = 0;
  return 0;
}

void rz_race_state_destroy(rz_race_state *s) {
  free(s->buckets);
  s->buckets = NULL;
  s->next_tid = 0;
}

void rz_thread_init(rz_race_state *s, rz_thread *t) {
  t->tid = s->next_tid++;
  rz_vc_init(&t->clock);
  rz_vc_tick(&t->clock, t->tid); // own epoch starts at 1 (bounds-checked)
}

// Locate the shadow bucket for an 8-byte word, open-addressing with linear
// probing over a power-of-two table. With `create`, claim the first empty slot;
// returns NULL only if the table is completely full (then the caller skips the
// access -- a possible missed race, never a false report).
static rz_bucket *find_bucket(rz_race_state *s, uintptr_t word, int create) {
  uintptr_t key = word + 1; // 0 is reserved for "empty"
  size_t mask = RZ_RACE_BUCKETS - 1;
  size_t h = (size_t)(word * 0x9E3779B97F4A7C15ull) & mask;
  for (size_t i = 0; i < RZ_RACE_BUCKETS; i++) {
    rz_bucket *b = &s->buckets[(h + i) & mask];
    if (b->key == key)
      return b;
    if (b->key == 0) {
      if (!create)
        return NULL;
      b->key = key;
      return b;
    }
  }
  return NULL; // table full
}

int rz_race_access(rz_race_state *s, rz_thread *t, uintptr_t addr,
                   int is_write) {
  uintptr_t word = addr >> 3;
  rz_bucket *b = find_bucket(s, word, 1);
  if (!b)
    return 0; // table full: skip (false negative, never false positive)

  rz_epoch_t cur_epoch =
      (t->tid >= 0 && t->tid < RZ_RACE_MAX_THREADS) ? t->clock.t[t->tid] : 0;

  // Check the new access against every recorded cell for this location.
  int raced = 0;
  for (int i = 0; i < RZ_RACE_CELLS; i++)
    if (rz_race_check(&b->cells[i], t->tid, &t->clock, is_write))
      raced = 1;

  // Record the access. Prefer overwriting a cell already owned by this thread
  // (keep only its latest epoch), then a free cell, then evict cell 0.
  int slot = -1, freeslot = -1;
  for (int i = 0; i < RZ_RACE_CELLS; i++) {
    if (b->cells[i].valid && b->cells[i].tid == t->tid) {
      slot = i;
      break;
    }
    if (!b->cells[i].valid && freeslot < 0)
      freeslot = i;
  }
  if (slot < 0)
    slot = (freeslot >= 0) ? freeslot : 0;

  rz_access *c = &b->cells[slot];
  // Two accesses by this thread at the SAME epoch (no sync between them) can be
  // safely combined: a read+write at one epoch is a write at that epoch. Across
  // epochs we just keep the latest (dropping the older access -- a false
  // negative, never a false positive, since merging a stale write into a newer
  // epoch would over-report).
  if (c->valid && c->tid == t->tid && c->epoch == cur_epoch)
    is_write = c->is_write || is_write;
  c->valid = 1;
  c->tid = t->tid;
  c->epoch = cur_epoch;
  c->is_write = is_write;
  return raced;
}

void rz_sync_init(rz_sync *m) { rz_vc_init(&m->clock); }

void rz_mutex_release(rz_thread *t, rz_sync *m) {
  // Publish the releasing thread's clock into the lock, then advance the
  // thread's own epoch so its post-release actions are not ordered into the lock
  // (a later acquirer sees everything up to here, but nothing after).
  rz_vc_join(&m->clock, &t->clock);
  rz_vc_tick(&t->clock, t->tid);
}

void rz_mutex_acquire(rz_thread *t, rz_sync *m) {
  // Import the lock's published clock: everything the last releaser did before
  // releasing now happens-before this thread.
  rz_vc_join(&t->clock, &m->clock);
}

void rz_thread_create(rz_race_state *s, rz_thread *parent, rz_thread *child) {
  child->tid = s->next_tid++;
  rz_vc_copy(&child->clock, &parent->clock); // child inherits parent's history
  rz_vc_tick(&child->clock, child->tid);     // child starts its own epoch
  rz_vc_tick(&parent->clock, parent->tid);   // parent moves past the fork point
}

void rz_thread_join(rz_thread *parent, const rz_thread *child) {
  rz_vc_join(&parent->clock, &child->clock); // child's history orders before
}
