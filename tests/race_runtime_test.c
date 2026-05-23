// Real-thread test for the race-detector runtime glue (runtime/redzone_race_rt).
//
// Unlike race_engine_test.c (which drives the engine by hand), this spins up
// actual pthreads through the runtime's wrappers. It is still DETERMINISTIC in
// its assertions: happens-before is timing-independent, so a correctly
// synchronized program reports zero races on every run, and a genuinely racy one
// always reports at least one -- regardless of how the threads interleave. The
// CI script runs it many times to make that determinism a regression gate.
#include "redzone_race_rt.h"

#include <pthread.h>
#include <stdio.h>

static int failures = 0;

static void check(const char *name, int got, int want) {
  if (got == want) {
    printf("PASS  %s\n", name);
  } else {
    printf("FAIL  %s (got %d, want %d)\n", name, got, want);
    failures++;
  }
}

#define ITERS 2000

// --- Scenario 1: writes to a shared word, protected by a mutex -> no race. ---
static long safe_shared;
static pthread_mutex_t safe_mtx = PTHREAD_MUTEX_INITIALIZER;

static void *safe_worker(void *arg) {
  (void)arg;
  for (int i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&safe_mtx);
    rz_rt_mutex_lock(&safe_mtx);
    rz_rt_write(&safe_shared, sizeof safe_shared, __FILE__, __LINE__);
    safe_shared++;
    rz_rt_mutex_unlock(&safe_mtx);
    pthread_mutex_unlock(&safe_mtx);
  }
  return NULL;
}

// --- Scenario 2: create/join handoff of one word -> no race. ---
static long handoff;

static void *handoff_worker(void *arg) {
  (void)arg;
  rz_rt_write(&handoff, sizeof handoff, __FILE__, __LINE__); // after parent's write
  handoff = 2;
  return NULL;
}

// --- Scenario 3: writes to a shared word with NO synchronization -> race. ---
static long racy_shared;

static void *racy_worker(void *arg) {
  (void)arg;
  for (int i = 0; i < ITERS; i++) {
    rz_rt_write(&racy_shared, sizeof racy_shared, __FILE__, __LINE__);
    racy_shared++;
  }
  return NULL;
}

int main(void) {
  rz_rt_init();

  // 1. Mutex-protected concurrent writes: every cross-thread pair is ordered by
  //    the lock, so the detector must report nothing.
  {
    pthread_t a, b;
    rz_rt_pthread_create(&a, NULL, safe_worker, NULL);
    rz_rt_pthread_create(&b, NULL, safe_worker, NULL);
    rz_rt_pthread_join(a, NULL);
    rz_rt_pthread_join(b, NULL);
    check("rt: mutex-protected writes -> no race", rz_rt_race_count() == 0, 1);
  }

  // 2. Create/join handoff: the parent's pre-create write, the child's write,
  //    and the parent's post-join write are all totally ordered -> no race.
  {
    unsigned long before = rz_rt_race_count();
    rz_rt_write(&handoff, sizeof handoff, __FILE__, __LINE__); // before create
    handoff = 1;
    pthread_t c;
    rz_rt_pthread_create(&c, NULL, handoff_worker, NULL);
    rz_rt_pthread_join(c, NULL);
    rz_rt_write(&handoff, sizeof handoff, __FILE__, __LINE__); // after join
    handoff = 3;
    check("rt: create/join handoff -> no race",
          rz_rt_race_count() == before, 1);
  }

  // 3. Unsynchronized concurrent writes: the two threads are concurrent (only a
  //    common ancestor at create, no edge between them) -> at least one race.
  {
    unsigned long before = rz_rt_race_count();
    pthread_t d, e;
    rz_rt_pthread_create(&d, NULL, racy_worker, NULL);
    rz_rt_pthread_create(&e, NULL, racy_worker, NULL);
    rz_rt_pthread_join(d, NULL);
    rz_rt_pthread_join(e, NULL);
    check("rt: unsynchronized writes -> race",
          rz_rt_race_count() > before, 1);
  }

  printf("\n");
  if (failures == 0) {
    printf("race-runtime: all scenarios passed\n");
    return 0;
  }
  printf("race-runtime: %d scenario(s) FAILED\n", failures);
  return 1;
}
