// Expected: global-buffer-overflow.
// Writes past the end of a fixed-size internal (static) global array. The store
// to g[4] lands in the global's trailing red zone, so redzone should abort.
#include <stdio.h>

static int g[4] = {1, 2, 3, 4};

int main(void) {
  for (int i = 0; i < 4; i++)
    g[i] += 1;

  g[4] = 99; // <-- out of bounds on a global

  printf("%d\n", g[4]);
  return 0;
}
