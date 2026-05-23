// Expected: clean exit.
// Thread-safety stress test: many threads concurrently malloc/use/free disjoint
// blocks. With a correct allocation-table lock and race-free shadow, this must
// run clean -- no false positive and no crash from a corrupted table. (A data
// race in redzone itself would tend to surface here as a spurious error or a
// segfault.)
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { THREADS = 8, ITERS = 10000 };

static void *worker(void *arg) {
  long acc = 0;
  unsigned seed = (unsigned)(uintptr_t)arg;
  for (int it = 0; it < ITERS; it++) {
    int n = 4 + (int)((seed + (unsigned)it) & 7); // 4..11 ints
    int *a = malloc((size_t)n * sizeof(int));
    if (!a)
      return (void *)1;
    for (int i = 0; i < n; i++)
      a[i] = i + it;
    for (int i = 0; i < n; i++)
      acc += a[i]; // all in-bounds
    free(a);
  }
  return (void *)acc;
}

int main(void) {
  pthread_t t[THREADS];
  for (int i = 0; i < THREADS; i++)
    if (pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i) != 0)
      return 1;
  long total = 0;
  for (int i = 0; i < THREADS; i++) {
    void *r;
    pthread_join(t[i], &r);
    total += (long)r;
  }
  printf("%ld\n", total);
  return 0;
}
