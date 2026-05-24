// ThreadSanitizer validation of the race RUNTIME's OWN locking (the sharded
// shadow + the meta lock). Several threads drive the detector concurrently on a
// workload with NO target-level races: each thread hammers its own disjoint slot
// (exercising different shards in parallel) and bumps a shared counter under a
// mutex (exercising the sync registry and one shard concurrently). Because the
// workload is race-free, ThreadSanitizer should report nothing -- any warning it
// does emit is a data race INSIDE the detector, i.e. a locking bug in the
// sharding. The detector must also report zero races on this input.
#include "redzone_race_rt.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define NT 6
#define ITERS 20000

// Spaced an entire cache line apart so different threads hit different words
// (and, via the hash, different shards).
static long slots[NT * 8];
static long shared;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *p) {
  long id = (long)(intptr_t)p;
  long *mine = &slots[id * 8];
  for (int i = 0; i < ITERS; i++) {
    rz_rt_write(mine, sizeof *mine, __FILE__, __LINE__); // own slot -> no race
    *mine += i;
    rz_rt_pthread_mutex_lock(&mtx);
    rz_rt_write(&shared, sizeof shared, __FILE__, __LINE__); // mutex-ordered
    shared += 1;
    rz_rt_pthread_mutex_unlock(&mtx);
  }
  return NULL;
}

int main(void) {
  rz_rt_init();
  pthread_t t[NT];
  for (long k = 0; k < NT; k++)
    rz_rt_pthread_create(&t[k], NULL, worker, (void *)(intptr_t)k);
  for (int k = 0; k < NT; k++)
    rz_rt_pthread_join(t[k], NULL);

  unsigned long races = rz_rt_race_count();
  if (races != 0) {
    fprintf(stderr, "FAIL: %lu spurious race(s) on a race-free workload\n", races);
    return 1;
  }
  printf("race-tsan: no detector-internal races; shared=%ld\n", shared);
  return 0;
}
