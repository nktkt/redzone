// Mutual exclusion via pthread_mutex_trylock spun until it succeeds. A
// successful trylock establishes the same acquire edge as a plain lock, so the
// increments are ordered and the detector must report NO race -- exercising the
// conditional (success-only) acquire of the trylock wrapper.
#include <pthread.h>
#include <stdio.h>

static long counter;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
  (void)arg;
  for (int i = 0; i < 50000; i++) {
    while (pthread_mutex_trylock(&m) != 0) {
      /* contended: spin until we get the lock */
    }
    counter++;
    pthread_mutex_unlock(&m);
  }
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
