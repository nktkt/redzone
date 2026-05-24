//===- redzone_race_rt.c - real-execution glue (see redzone_race_rt.h) ----===//

#include "redzone_race_rt.h"

#include "redzone_race.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

//===----------------------------------------------------------------------===//
// Global detector state.
//
// The shadow is SHARDED into RZ_NSHARD independent sub-tables, each with its own
// lock and keyed by the access address, so threads touching unrelated locations
// don't serialize on a single mutex (the dominant cost measured by
// scripts/bench_race.sh). Everything else -- the sync registry, the thread
// registry, the tid counter, and the report state -- is lower-frequency and
// shares one `g_meta_lock`. The two lock kinds are NEVER held at the same time
// (a shard lock is always released before g_meta_lock is taken), so they cannot
// deadlock. A thread's own vector clock lives in TLS and is touched only by that
// thread (writes at create happen before the child runs; reads at join happen
// after it ends), so it needs no lock at all.
//===----------------------------------------------------------------------===//

#define RZ_NSHARD 64u // power of two
static rz_race_state g_shard[RZ_NSHARD];
static pthread_mutex_t g_shard_lock[RZ_NSHARD];
static rz_race_state g_meta; // holds only next_tid (no shadow); under g_meta_lock
static pthread_mutex_t g_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_self_key; // TLS: this thread's rz_thread*
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static unsigned long g_races;    // total races reported (under g_meta_lock)
static int g_verbose;            // report every race, no dedup? (REDZONE_RACE_VERBOSE)

#define META_LOCK() pthread_mutex_lock(&g_meta_lock)
#define META_UNLOCK() pthread_mutex_unlock(&g_meta_lock)

// Route an 8-byte word to a shard. A different bit range than find_bucket's hash
// (which masks the low bits) to decorrelate shard choice from bucket choice.
static unsigned shard_of(uintptr_t word) {
  return (unsigned)((word * 0x9E3779B97F4A7C15ull) >> 40) & (RZ_NSHARD - 1);
}

// Dedup detailed race reports by (line, prev-line) so a hot racy loop prints a
// handful of distinct reports, not thousands. The exit summary still counts all
// races. Decided under g_meta_lock; the printing happens after the lock is released.
#define RZ_RACE_REPORT_CAP 16
static int g_report_lines[RZ_RACE_REPORT_CAP][2];
static int g_report_n;

static int should_report(int line, int prev_line) { // call under g_meta_lock
  if (g_verbose)
    return 1;
  for (int i = 0; i < g_report_n; i++)
    if (g_report_lines[i][0] == line && g_report_lines[i][1] == prev_line)
      return 0; // this pair was already reported
  if (g_report_n >= RZ_RACE_REPORT_CAP)
    return 0; // cap reached; the exit summary still counts these
  g_report_lines[g_report_n][0] = line;
  g_report_lines[g_report_n][1] = prev_line;
  g_report_n++;
  return 1;
}

static void print_race(int is_write, int tid, uintptr_t addr, const char *file,
                       int line, const rz_access *prev) {
  fflush(stdout); // keep the program's own output ahead of the report
  fprintf(stderr, "==redzone WARNING: data race\n");
  fprintf(stderr, "  %s by thread %d at %s:%d\n", is_write ? "write" : "read",
          tid, file ? file : "<unknown>", line);
  if (prev && prev->valid)
    fprintf(stderr, "  previous %s by thread %d at %s:%d\n",
            prev->is_write ? "write" : "read", prev->tid,
            prev->file ? prev->file : "<unknown>", prev->line);
  fprintf(stderr, "  address %p\n", (void *)addr);
}

//===----------------------------------------------------------------------===//
// pthread_t -> child rz_thread* registry, for join. Small grow-only array of
// slots that are reused after a join. All access is under g_lock.
//===----------------------------------------------------------------------===//

typedef struct {
  pthread_t thr;
  rz_thread *self;
  int used;
} rz_thr_ent;

static rz_thr_ent *g_threads;
static size_t g_threads_cap;

static void reg_thread(pthread_t thr, rz_thread *self) {
  for (size_t i = 0; i < g_threads_cap; i++) {
    if (!g_threads[i].used) {
      g_threads[i].thr = thr;
      g_threads[i].self = self;
      g_threads[i].used = 1;
      return;
    }
  }
  size_t old = g_threads_cap;
  size_t cap = old ? old * 2 : 16;
  rz_thr_ent *grown = realloc(g_threads, cap * sizeof *grown);
  if (!grown)
    return; // out of memory: drop the mapping (join won't add an edge)
  g_threads = grown;
  for (size_t i = old; i < cap; i++)
    g_threads[i].used = 0;
  g_threads_cap = cap;
  g_threads[old].thr = thr;
  g_threads[old].self = self;
  g_threads[old].used = 1;
}

// Find the handle for `thr`, free its slot, and return it (NULL if unknown).
static rz_thread *take_thread(pthread_t thr) {
  for (size_t i = 0; i < g_threads_cap; i++) {
    if (g_threads[i].used && pthread_equal(g_threads[i].thr, thr)) {
      g_threads[i].used = 0;
      return g_threads[i].self;
    }
  }
  return NULL;
}

//===----------------------------------------------------------------------===//
// address -> rz_sync registry, SHARDED. Each lock/atomic/cond object lives in
// one shard (keyed by its address); a shard's grow-only array and clocks are
// protected by that shard's lock. Sharding keeps unrelated locks and atomics
// from serializing on a single registry lock -- the same parallelism win as the
// shadow shards, but for synchronization (notably atomic-heavy lock-free code).
//===----------------------------------------------------------------------===//

#define RZ_NSYNC 16u // power of two

typedef struct {
  uintptr_t key; // object address
  rz_sync sync;
} rz_sync_ent;

typedef struct {
  rz_sync_ent *ents;
  size_t cap, len;
} rz_sync_shard;

static rz_sync_shard g_sync[RZ_NSYNC];
static pthread_mutex_t g_sync_lock[RZ_NSYNC];

static unsigned sync_shard_of(uintptr_t key) {
  return (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 36) & (RZ_NSYNC - 1);
}

// Find-or-create the sync object for `key` within shard `sh`. Call under the
// shard's lock; the returned pointer is valid only until the lock is released
// (a later insert may realloc the array).
static rz_sync *sync_for_in(rz_sync_shard *sh, uintptr_t key) {
  for (size_t i = 0; i < sh->len; i++)
    if (sh->ents[i].key == key)
      return &sh->ents[i].sync;
  if (sh->len == sh->cap) {
    size_t cap = sh->cap ? sh->cap * 2 : 8;
    rz_sync_ent *grown = realloc(sh->ents, cap * sizeof *grown);
    if (!grown)
      return NULL; // OOM: caller skips the edge (may miss races, never invents)
    sh->ents = grown;
    sh->cap = cap;
  }
  rz_sync_ent *e = &sh->ents[sh->len++];
  e->key = key;
  rz_sync_init(&e->sync);
  return &e->sync;
}

//===----------------------------------------------------------------------===//
// Init and per-thread handle.
//===----------------------------------------------------------------------===//

static void init_once(void) {
  pthread_key_create(&g_self_key, NULL); // handles are freed by us, not the key
  // Shard the shadow so total capacity stays ~constant: each shard gets
  // RZ_RACE_BUCKETS / RZ_NSHARD buckets.
  for (unsigned i = 0; i < RZ_NSHARD; i++) {
    rz_race_state_init_n(&g_shard[i], RZ_RACE_BUCKETS / RZ_NSHARD);
    pthread_mutex_init(&g_shard_lock[i], NULL);
  }
  for (unsigned i = 0; i < RZ_NSYNC; i++)
    pthread_mutex_init(&g_sync_lock[i], NULL);
  g_meta.buckets = NULL; // g_meta carries only next_tid (no shadow)
  g_meta.nbuckets = 0;
  g_meta.next_tid = 0;
  g_verbose = getenv("REDZONE_RACE_VERBOSE") != NULL;
  rz_thread *root = calloc(1, sizeof *root);
  if (root) {
    rz_thread_init(&g_meta, root); // tid 0 = the calling (root) thread
    pthread_setspecific(g_self_key, root);
  }
}

void rz_rt_init(void) { pthread_once(&g_once, init_once); }

// The calling thread's logical clock. Threads spawned via rz_rt_pthread_create
// get theirs installed by the trampoline before any access; the root thread gets
// its in init. A thread created some other way is registered lazily here -- its
// create edge is then unknown, so this is the one unsound corner, avoided once
// pthread_create is intercepted everywhere.
static rz_thread *self(void) {
  rz_rt_init();
  rz_thread *s = pthread_getspecific(g_self_key);
  if (!s) {
    s = calloc(1, sizeof *s);
    if (!s)
      return NULL;
    META_LOCK();
    rz_thread_init(&g_meta, s);
    META_UNLOCK();
    pthread_setspecific(g_self_key, s);
  }
  return s;
}

//===----------------------------------------------------------------------===//
// Thread lifecycle.
//===----------------------------------------------------------------------===//

typedef struct {
  void *(*start)(void *);
  void *arg;
  rz_thread *self;
} rz_tramp;

static void *trampoline(void *p) {
  rz_tramp *t = p;
  pthread_setspecific(g_self_key, t->self); // install clock before user code
  void *(*start)(void *) = t->start;
  void *arg = t->arg;
  free(t);
  return start(arg);
}

int rz_rt_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*start_routine)(void *), void *arg) {
  rz_thread *parent = self();
  rz_thread *child = calloc(1, sizeof *child);
  rz_tramp *tr = malloc(sizeof *tr);
  if (!child || !tr) {
    free(child);
    free(tr);
    return pthread_create(thread, attr, start_routine, arg); // untracked
  }
  META_LOCK();
  rz_thread_create(&g_meta, parent, child); // parent -> child ordering edge
  META_UNLOCK();
  tr->start = start_routine;
  tr->arg = arg;
  tr->self = child;
  int rc = pthread_create(thread, attr, trampoline, tr);
  if (rc == 0) {
    META_LOCK();
    reg_thread(*thread, child);
    META_UNLOCK();
  } else {
    free(tr);
    free(child);
  }
  return rc;
}

int rz_rt_pthread_join(pthread_t thread, void **retval) {
  rz_thread *parent = self();
  int rc = pthread_join(thread, retval);
  if (rc == 0) {
    META_LOCK();
    rz_thread *child = take_thread(thread);
    if (child)
      rz_thread_join(parent, child); // child -> parent ordering edge
    META_UNLOCK();
    free(child);
  }
  return rc;
}

//===----------------------------------------------------------------------===//
// Synchronization and memory events.
//===----------------------------------------------------------------------===//

void rz_rt_mutex_lock(void *mutex) {
  rz_thread *s = self();
  if (!s)
    return;
  unsigned sh = sync_shard_of((uintptr_t)mutex);
  pthread_mutex_lock(&g_sync_lock[sh]);
  rz_sync *m = sync_for_in(&g_sync[sh], (uintptr_t)mutex);
  if (m)
    rz_mutex_acquire(s, m);
  pthread_mutex_unlock(&g_sync_lock[sh]);
}

void rz_rt_mutex_unlock(void *mutex) {
  rz_thread *s = self();
  if (!s)
    return;
  unsigned sh = sync_shard_of((uintptr_t)mutex);
  pthread_mutex_lock(&g_sync_lock[sh]);
  rz_sync *m = sync_for_in(&g_sync[sh], (uintptr_t)mutex);
  if (m)
    rz_mutex_release(s, m);
  pthread_mutex_unlock(&g_sync_lock[sh]);
}

int rz_rt_pthread_mutex_lock(pthread_mutex_t *mutex) {
  int rc = pthread_mutex_lock(mutex);
  rz_rt_mutex_lock(mutex); // record the acquire once we actually hold the lock
  return rc;
}

int rz_rt_pthread_mutex_unlock(pthread_mutex_t *mutex) {
  rz_rt_mutex_unlock(mutex); // publish our clock while we still hold the lock
  return pthread_mutex_unlock(mutex);
}

int rz_rt_pthread_mutex_trylock(pthread_mutex_t *mutex) {
  int rc = pthread_mutex_trylock(mutex);
  if (rc == 0)
    rz_rt_mutex_lock(mutex); // only an acquire when we actually got the lock
  return rc;
}

// rwlocks reuse the mutex sync primitives (acquire on lock, release on unlock),
// keyed by the rwlock's address. See the header for why this is sound.
int rz_rt_pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
  int rc = pthread_rwlock_rdlock(rwlock);
  rz_rt_mutex_lock(rwlock);
  return rc;
}

int rz_rt_pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
  int rc = pthread_rwlock_wrlock(rwlock);
  rz_rt_mutex_lock(rwlock);
  return rc;
}

int rz_rt_pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
  int rc = pthread_rwlock_tryrdlock(rwlock);
  if (rc == 0)
    rz_rt_mutex_lock(rwlock);
  return rc;
}

int rz_rt_pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
  int rc = pthread_rwlock_trywrlock(rwlock);
  if (rc == 0)
    rz_rt_mutex_lock(rwlock);
  return rc;
}

int rz_rt_pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
  rz_rt_mutex_unlock(rwlock); // publish before releasing (read or write unlock)
  return pthread_rwlock_unlock(rwlock);
}

// A condvar wait is, for happens-before purposes, an unlock of the mutex
// followed (on wake) by a re-lock. Record both around the real call so the
// edge the re-acquire carries (the signaller's writes, published via the mutex)
// is not lost. The cond object itself needs no clock.
int rz_rt_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  rz_rt_mutex_unlock(mutex);
  int rc = pthread_cond_wait(cond, mutex);
  rz_rt_mutex_lock(mutex);
  return rc;
}

int rz_rt_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                 const struct timespec *abstime) {
  rz_rt_mutex_unlock(mutex);
  int rc = pthread_cond_timedwait(cond, mutex, abstime);
  rz_rt_mutex_lock(mutex);
  return rc;
}

// Atomic ops reuse the record-only sync primitives, keyed by the location's
// address: an atomic read acquires the location's clock, an atomic write
// publishes into it. See the header for the soundness argument.
void rz_rt_atomic_acquire(const volatile void *addr) {
  rz_rt_mutex_lock((void *)(uintptr_t)addr);
}

void rz_rt_atomic_release(const volatile void *addr) {
  rz_rt_mutex_unlock((void *)(uintptr_t)addr);
}

static void access_range(uintptr_t addr, size_t size, int is_write,
                         const char *file, int line) {
  rz_thread *s = self();
  if (!s)
    return;
  if (size == 0)
    size = 1;
  uintptr_t first = addr >> 3;
  uintptr_t last = (addr + size - 1) >> 3;
  int tid = s->tid;
  unsigned long hits = 0;
  uintptr_t hit_word = 0;
  rz_access prev; // the conflicting prior access, for the report
  int do_report = 0;
  // Each 8-byte word is checked under its own shard lock, one at a time, so
  // accesses to unrelated locations run in parallel.
  for (uintptr_t w = first; w <= last; w++) {
    unsigned sh = shard_of(w);
    rz_access p;
    pthread_mutex_lock(&g_shard_lock[sh]);
    int raced =
        rz_race_access_loc(&g_shard[sh], s, w << 3, is_write, file, line, &p);
    pthread_mutex_unlock(&g_shard_lock[sh]);
    if (raced) {
      if (!hits) {
        hit_word = w << 3;
        prev = p;
      }
      hits++;
    }
  }
  if (hits) {
    META_LOCK();
    g_races += hits;
    do_report = should_report(line, prev.line);
    META_UNLOCK();
    if (do_report)
      print_race(is_write, tid, hit_word, file, line, &prev);
  }
}

void rz_rt_read(const volatile void *addr, size_t size, const char *file,
                int line) {
  access_range((uintptr_t)addr, size, 0, file, line);
}

void rz_rt_write(const volatile void *addr, size_t size, const char *file,
                 int line) {
  access_range((uintptr_t)addr, size, 1, file, line);
}

unsigned long rz_rt_race_count(void) {
  META_LOCK();
  unsigned long r = g_races;
  META_UNLOCK();
  return r;
}
