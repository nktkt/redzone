// A one-slot bounded buffer: a producer and a consumer hand off 2000 values
// through a shared box, coordinated by a mutex and two condition variables. All
// access to the shared state is under the mutex, and the handoff blocks on the
// condvars, so the detector must report NO race.
//
// This is the test that fails without correct condvar handling: pthread_cond_wait
// releases and re-acquires the mutex *inside* libc, so if the re-acquire edge is
// not recorded, the consumer's read of `box` looks unordered w.r.t. the
// producer's write -> a false positive. The producer uses cond_wait and the
// consumer uses cond_timedwait, exercising both wrappers.
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define N 2000

static long box;
static int full;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

static void *producer(void *arg) {
  (void)arg;
  for (long i = 1; i <= N; i++) {
    pthread_mutex_lock(&m);
    while (full)
      pthread_cond_wait(&not_full, &m);
    box = i; // write under the mutex
    full = 1;
    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&m);
  }
  return NULL;
}

static void *consumer(void *arg) {
  (void)arg;
  long sum = 0;
  for (int i = 0; i < N; i++) {
    pthread_mutex_lock(&m);
    while (!full) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 5; // generous; the signal arrives long before this
      pthread_cond_timedwait(&not_empty, &m, &ts);
    }
    sum += box; // read under the mutex -> ordered after the producer's write
    full = 0;
    pthread_cond_signal(&not_full);
    pthread_mutex_unlock(&m);
  }
  printf("sum = %ld\n", sum);
  return NULL;
}

int main(void) {
  pthread_t p, c;
  pthread_create(&p, NULL, producer, NULL);
  pthread_create(&c, NULL, consumer, NULL);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  return 0;
}
