// A reader/writer-lock protecting a shared value: two reader threads read it
// under a read-lock and one writer mutates it under a write-lock. Every access
// is ordered by the rwlock, so the detector must report NO race -- exercising
// pthread_rwlock_rdlock/wrlock/unlock.
#include <pthread.h>
#include <stdio.h>

static long shared;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;

static void *reader(void *arg) {
  (void)arg;
  long acc = 0;
  for (int i = 0; i < 50000; i++) {
    pthread_rwlock_rdlock(&rw);
    acc += shared; // read under the read-lock
    pthread_rwlock_unlock(&rw);
  }
  return (void *)acc;
}

static void *writer(void *arg) {
  (void)arg;
  for (int i = 0; i < 50000; i++) {
    pthread_rwlock_wrlock(&rw);
    shared++; // write under the write-lock
    pthread_rwlock_unlock(&rw);
  }
  return NULL;
}

int main(void) {
  pthread_t r1, r2, w;
  pthread_create(&r1, NULL, reader, NULL);
  pthread_create(&r2, NULL, reader, NULL);
  pthread_create(&w, NULL, writer, NULL);
  pthread_join(r1, NULL);
  pthread_join(r2, NULL);
  pthread_join(w, NULL);
  printf("shared = %ld\n", shared);
  return 0;
}
