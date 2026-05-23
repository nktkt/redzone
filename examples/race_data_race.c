// A genuine data race: two threads increment the same global with no
// synchronization. Built in redzone's race mode, the detector must report it.
// (Detection is timing-independent: the two increments are concurrent under the
// memory model regardless of how the threads interleave, so the race is found on
// every run.)
#include <pthread.h>
#include <stdio.h>

static long counter; // shared, unprotected

static void *worker(void *arg) {
  (void)arg;
  for (int i = 0; i < 100000; i++)
    counter++; // unsynchronized read-modify-write -> races with the other thread
  return NULL;
}

int main(void) {
  pthread_t a, b;
  pthread_create(&a, NULL, worker, NULL);
  pthread_create(&b, NULL, worker, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
  printf("counter = %ld\n", counter);
  return 0;
}
