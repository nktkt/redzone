// Expected: memory-leak.
// Leaks several blocks from the SAME allocation site. The text report collapses
// them into a single line with a count, rather than printing one line per block.
#include <stdlib.h>

int main(void) {
  for (int i = 0; i < 5; i++) {
    int *p = malloc(8 * sizeof(int)); // one site, leaked 5 times
    if (!p)
      return 1;
    p[0] = i; // touch it so the allocation isn't optimized away
  }
  return 0;
}
