// Race-mode microbenchmark: single-threaded, to measure the PER-ACCESS cost of
// the detector (the runtime call + lock + happens-before bookkeeping) with no
// lock contention. `volatile` forces a real load/store each iteration (defeating
// hoisting); the trip count comes from argv so neither build can fold it away;
// the result is printed so nothing is dead-code-eliminated.
#include <stdio.h>
#include <stdlib.h>

static volatile long sink;

int main(int argc, char **argv) {
  long iters = (argc > 1) ? atol(argv[1]) : 1000000;
  long acc = 0;
  for (long i = 0; i < iters; i++) {
    sink = i;    // instrumented write
    acc += sink; // instrumented read
  }
  printf("%ld\n", acc);
  return 0;
}
