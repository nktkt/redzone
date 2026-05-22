// Expected: memory-leak.
// Allocates a block and returns without freeing it. redzone reports the
// un-freed allocation at program exit and exits with a nonzero status.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(8 * sizeof(int));
  if (!buf)
    return 1;

  for (int i = 0; i < 8; i++)
    buf[i] = i;

  printf("done\n");
  return 0; // no free -> leak
}
