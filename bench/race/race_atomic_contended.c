// Race-mode microbenchmark: several threads, each doing atomic RMWs on its OWN
// cache-line-padded atomic counter (so no data race, no false sharing). In race
// mode every atomic op is modeled as acquire/release on the location's sync
// object; if the sync registry is behind one lock, the threads serialize there
// even though their counters are unrelated -- this benchmark exposes that, and a
// sharded sync registry should bring it back toward parallel.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NT 4

typedef struct {
  _Atomic long v;
  char pad[56]; // pad to a cache line
} slot_t;

static slot_t slots[NT];
static long iters;

static void *worker(void *p) {
  long id = (long)(intptr_t)p;
  long acc = 0;
  for (long i = 0; i < iters; i++)
    acc += atomic_fetch_add_explicit(&slots[id].v, 1, memory_order_relaxed);
  return (void *)(intptr_t)acc;
}

int main(int argc, char **argv) {
  iters = (argc > 1) ? atol(argv[1]) : 200000;
  pthread_t t[NT];
  for (long k = 0; k < NT; k++)
    pthread_create(&t[k], NULL, worker, (void *)(intptr_t)k);
  long sum = 0;
  for (int k = 0; k < NT; k++) {
    void *r;
    pthread_join(t[k], &r);
    sum += (long)(intptr_t)r;
  }
  printf("%ld\n", sum);
  return 0;
}
