// The same shared counter as race_data_race.c, but every access is protected by
// a mutex. The lock/unlock create happens-before edges, so the accesses are
// ordered and the detector must report NO race -- the property that matters most
// (a false race on correct code would destroy trust).
#include <pthread.h>
#include <stdio.h>

static long counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
  (void)arg;
  for (int i = 0; i < 100000; i++) {
    pthread_mutex_lock(&lock);
    counter++; // serialized by the lock -> ordered, not a race
    pthread_mutex_unlock(&lock);
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
