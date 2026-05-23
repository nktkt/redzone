// Expected: global-buffer-overflow.
// A non-static (external-linkage) global array. Unlike a static global, its
// address must be preserved so other translation units that reference it stay
// correct, so redzone guards it with a TRAILING red zone only. Writing g[4]
// lands in that red zone.
#include <stdio.h>

int g[4] = {1, 2, 3, 4}; // external linkage (no `static`)

int main(void) {
  for (int i = 0; i < 4; i++)
    g[i] += 1;

  g[4] = 99; // <-- out of bounds, into the trailing red zone

  printf("%d\n", g[4]);
  return 0;
}
