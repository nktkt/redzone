// Release/acquire publication: the producer writes (non-atomic) data, then sets
// an atomic flag with release; the consumer spins on the flag with acquire, then
// reads the data. The atomic flag orders the data accesses, so the detector must
// report NO race.
//
// This is the program that false-positives WITHOUT atomic handling: treated as
// plain accesses, the flag's store and load would race, and the data read would
// look unordered. Modeling the atomics as release/acquire fixes both.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static long data;        // plain, published through the flag
static atomic_int ready; // the release/acquire flag

static void *producer(void *arg) {
  (void)arg;
  data = 42; // non-atomic write
  atomic_store_explicit(&ready, 1, memory_order_release);
  return NULL;
}

static void *consumer(void *arg) {
  (void)arg;
  while (atomic_load_explicit(&ready, memory_order_acquire) == 0) {
    /* spin until the flag is set */
  }
  long v = data; // non-atomic read, ordered after the producer's write
  printf("v = %ld\n", v);
  return NULL;
}

int main(void) {
  pthread_t p, c;
  pthread_create(&c, NULL, consumer, NULL);
  pthread_create(&p, NULL, producer, NULL);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  return 0;
}
