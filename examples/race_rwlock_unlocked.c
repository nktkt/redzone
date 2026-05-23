// Guards against OVER-suppression: one thread writes a shared value correctly
// under a write-lock, while a rogue thread writes it with no lock at all. The
// rwlock modeling must not hide this -- the two writes are concurrent, so the
// detector must still report a race.
#include <pthread.h>
#include <stdio.h>

static long shared;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;

static void *locked_writer(void *arg) {
  (void)arg;
  for (int i = 0; i < 50000; i++) {
    pthread_rwlock_wrlock(&rw);
    shared++;
    pthread_rwlock_unlock(&rw);
  }
  return NULL;
}

static void *rogue_writer(void *arg) {
  (void)arg;
  for (int i = 0; i < 50000; i++)
    shared++; // no lock -> races with the locked writer
  return NULL;
}

int main(void) {
  pthread_t a, b;
  pthread_create(&a, NULL, locked_writer, NULL);
  pthread_create(&b, NULL, rogue_writer, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
  printf("shared = %ld\n", shared);
  return 0;
}
