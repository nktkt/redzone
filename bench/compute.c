// compute.c - compute-bound benchmark (EXPECT LOW overhead).
//
// A tight integer/math loop with a single small array touched once per outer
// iteration. The bulk of the work is in registers (a mixing hash over `acc`),
// so there is almost nothing for redzone to instrument: per-access checks are
// rare relative to total work. This anchors the LOW end of the overhead range,
// showing that redzone's cost tracks memory-access density, not raw CPU work.
#include <stdio.h>
#include <stdlib.h>

volatile long sink;

int main(int argc, char **argv) {
  // argv-controlled so the trip count is unknown at compile time.
  int iters = argc > 1 ? atoi(argv[1]) : 4000;
  int inner = 20000;
  int m = 64;

  // One small heap array, reused; the only instrumented memory traffic.
  unsigned *a = malloc(m * sizeof(unsigned));
  if (!a)
    return 1;
  for (int i = 0; i < m; i++)
    a[i] = (unsigned)(i * 2246822519u + 1u);

  unsigned long acc = 1469598103934665603ul; // FNV offset basis
  for (int it = 0; it < iters; it++) {
    // Heavy register-only mixing: no memory traffic to instrument here.
    for (int j = 0; j < inner; j++) {
      acc ^= (unsigned long)(j + it);
      acc *= 1099511628211ul; // FNV prime
      acc ^= acc >> 23;
      acc += acc << 7;
    }
    // A single data-dependent array access per outer iteration: a[i] feeds the
    // index, so it cannot be folded, but it is rare relative to the math above.
    int idx = (int)(acc % (unsigned long)m);
    acc += a[a[idx] % (unsigned)m];
  }

  sink = (long)acc;
  printf("%ld\n", (long)sink);
  free(a);
  return 0;
}
