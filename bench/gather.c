// gather.c - memory-bound benchmark (EXPECT HIGH overhead).
//
// Each iteration allocates a fresh array, fills it with indices in [0, m),
// then performs a *data-dependent* gather `sum += a[a[i]]`. The double
// indirection forces a real load to compute the second load's address, so
// the optimizer cannot fold the inner loop away in either build. Every
// access is in-bounds and the array is freed, so the instrumented build runs
// to completion. This stresses redzone's per-access __redzone_check the most.
#include <stdio.h>
#include <stdlib.h>

// volatile sink: writing the final result here keeps the whole computation
// live at -O2 in BOTH the baseline and the instrumented build.
volatile long sink;

int main(int argc, char **argv) {
  // Iteration count comes from argv so it is unknown at compile time; this
  // prevents the optimizer from unrolling/constant-folding the outer loop.
  int iters = argc > 1 ? atoi(argv[1]) : 4000;
  int m = 2000;
  long sum = 0;

  for (int it = 0; it < iters; it++) {
    int *a = malloc(m * sizeof(int));
    if (!a)
      return 1;
    // Fill with a pseudo-random permutation of indices in [0, m).
    for (int i = 0; i < m; i++)
      a[i] = (int)(((unsigned)i * 1103515245u + (unsigned)it) % (unsigned)m);
    // Data-dependent gather: a[i] is the load address for the second load.
    for (int i = 0; i < m; i++)
      sum += a[a[i]];
    free(a);
  }

  sink = sum;
  printf("%ld\n", (long)sink);
  return 0;
}
