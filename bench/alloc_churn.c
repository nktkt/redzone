// alloc_churn.c - allocator-churn benchmark.
//
// Each iteration performs many small malloc/free pairs and touches each block
// with a few data-dependent accesses. This exercises redzone's malloc/free
// wrapping (red-zone setup/teardown + shadow bookkeeping) far more than the
// gather benchmark, which allocates only once per outer iteration. All
// accesses are in-bounds and every block is freed.
#include <stdio.h>
#include <stdlib.h>

volatile long sink;

int main(int argc, char **argv) {
  // argv-controlled iteration count: unknown at compile time.
  int iters = argc > 1 ? atoi(argv[1]) : 200000;
  // Small block size so allocator/red-zone overhead dominates the work.
  int n = 16;
  long sum = 0;

  for (int it = 0; it < iters; it++) {
    int *a = malloc(n * sizeof(int));
    if (!a)
      return 1;
    // Fill with data-dependent values in [0, n).
    for (int i = 0; i < n; i++)
      a[i] = (int)(((unsigned)i * 2654435761u + (unsigned)it) % (unsigned)n);
    // Data-dependent gather over the tiny block so the loads can't be folded.
    for (int i = 0; i < n; i++)
      sum += a[a[i]];
    free(a);
  }

  sink = sum;
  printf("%ld\n", (long)sink);
  return 0;
}
