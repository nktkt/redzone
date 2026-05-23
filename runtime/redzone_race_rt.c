//===- redzone_race_rt.c - real-execution glue (see redzone_race_rt.h) ----===//

#include "redzone_race_rt.h"

#include "redzone_race.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

//===----------------------------------------------------------------------===//
// Global detector state, serialized by one lock.
//===----------------------------------------------------------------------===//

static rz_race_state g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_self_key; // TLS: this thread's rz_thread*
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static unsigned long g_races;    // total races reported (under g_lock)
static int g_verbose;            // print each race? (REDZONE_RACE_VERBOSE)

#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)

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
// mutex-address -> rz_sync registry. Grow-only; all access under g_lock.
//===----------------------------------------------------------------------===//

typedef struct {
  uintptr_t key; // mutex address; 0 means empty slot
  rz_sync sync;
} rz_sync_ent;

static rz_sync_ent *g_syncs;
static size_t g_syncs_cap, g_syncs_len;

static rz_sync *sync_for(uintptr_t key) {
  for (size_t i = 0; i < g_syncs_len; i++)
    if (g_syncs[i].key == key)
      return &g_syncs[i].sync;
  if (g_syncs_len == g_syncs_cap) {
    size_t cap = g_syncs_cap ? g_syncs_cap * 2 : 16;
    rz_sync_ent *grown = realloc(g_syncs, cap * sizeof *grown);
    if (!grown)
      return NULL; // OOM: caller skips the edge (may miss races, never invents)
    g_syncs = grown;
    g_syncs_cap = cap;
  }
  rz_sync_ent *e = &g_syncs[g_syncs_len++];
  e->key = key;
  rz_sync_init(&e->sync);
  return &e->sync;
}

//===----------------------------------------------------------------------===//
// Init and per-thread handle.
//===----------------------------------------------------------------------===//

static void init_once(void) {
  pthread_key_create(&g_self_key, NULL); // handles are freed by us, not the key
  rz_race_state_init(&g_state);
  g_verbose = getenv("REDZONE_RACE_VERBOSE") != NULL;
  rz_thread *root = calloc(1, sizeof *root);
  if (root) {
    rz_thread_init(&g_state, root); // tid 0 = the calling (root) thread
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
    LOCK();
    rz_thread_init(&g_state, s);
    UNLOCK();
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
  LOCK();
  rz_thread_create(&g_state, parent, child); // parent -> child ordering edge
  UNLOCK();
  tr->start = start_routine;
  tr->arg = arg;
  tr->self = child;
  int rc = pthread_create(thread, attr, trampoline, tr);
  if (rc == 0) {
    LOCK();
    reg_thread(*thread, child);
    UNLOCK();
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
    LOCK();
    rz_thread *child = take_thread(thread);
    if (child)
      rz_thread_join(parent, child); // child -> parent ordering edge
    UNLOCK();
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
  LOCK();
  rz_sync *m = sync_for((uintptr_t)mutex);
  if (m)
    rz_mutex_acquire(s, m);
  UNLOCK();
}

void rz_rt_mutex_unlock(void *mutex) {
  rz_thread *s = self();
  if (!s)
    return;
  LOCK();
  rz_sync *m = sync_for((uintptr_t)mutex);
  if (m)
    rz_mutex_release(s, m);
  UNLOCK();
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

static void access_range(uintptr_t addr, size_t size, int is_write) {
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
  LOCK();
  for (uintptr_t w = first; w <= last; w++) {
    if (rz_race_access(&g_state, s, w << 3, is_write)) {
      if (!hits)
        hit_word = w << 3;
      hits++;
    }
  }
  g_races += hits;
  UNLOCK();
  if (hits && g_verbose)
    fprintf(stderr,
            "[redzone] data race: %s by thread %d at %p\n",
            is_write ? "write" : "read", tid, (void *)hit_word);
}

void rz_rt_read(const volatile void *addr, size_t size) {
  access_range((uintptr_t)addr, size, 0);
}

void rz_rt_write(const volatile void *addr, size_t size) {
  access_range((uintptr_t)addr, size, 1);
}

unsigned long rz_rt_race_count(void) {
  LOCK();
  unsigned long r = g_races;
  UNLOCK();
  return r;
}
