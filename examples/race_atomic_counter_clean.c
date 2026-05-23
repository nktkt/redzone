// Two threads increment a shared counter with an atomic read-modify-write
// (atomic_fetch_add). Atomic accesses do not race with each other, so the
// detector must report NO race -- exercising the atomicrmw path.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_long counter;

static void *worker(void *arg) {
  (void)arg;
  for (int i = 0; i < 100000; i++)
    atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
  return NULL;
}

int main(void) {
  pthread_t a, b;
  pthread_create(&a, NULL, worker, NULL);
  pthread_create(&b, NULL, worker, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
  printf("counter = %ld\n", atomic_load(&counter));
  return 0;
}
