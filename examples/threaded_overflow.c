// Expected: heap-buffer-overflow.
// An overflow inside a worker thread must still be detected and abort the whole
// process -- instrumentation and the runtime work the same off the main thread.
#include <pthread.h>
#include <stdlib.h>

static void *worker(void *arg) {
  (void)arg;
  int *a = malloc(4 * sizeof(int));
  if (!a)
    return NULL;
  for (int i = 0; i < 4; i++)
    a[i] = i;
  a[4] = 99; // <-- out of bounds, on a non-main thread
  free(a);
  return NULL;
}

int main(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, worker, NULL) != 0)
    return 1;
  pthread_join(t, NULL);
  return 0;
}
