// Race-mode microbenchmark: several threads, each hammering its OWN cache-line-
// padded slot (so there is no data race and no false sharing). The baseline runs
// fully in parallel; the instrumented build serializes on the detector's single
// global lock, so the slowdown here exposes LOCK CONTENTION specifically -- the
// cost a sharded shadow lock would reduce. Compare its ratio to race_serial's
// (uncontended) to separate per-access cost from contention.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NT 4

typedef struct {
  volatile long v;
  char pad[56]; // pad to a cache line so threads don't false-share
} slot_t;

static slot_t slots[NT];
static long iters;

static void *worker(void *p) {
  long id = (long)(intptr_t)p;
  long acc = 0;
  for (long i = 0; i < iters; i++) {
    slots[id].v = i;    // instrumented write (this thread's own slot)
    acc += slots[id].v; // instrumented read
  }
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
