// Expected: use-after-free (WRITE).
// Writes into a block after it has been freed. The block is quarantined,
// so redzone should catch the store and abort.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int));
  if (!buf)
    return 1;

  buf[0] = 1;
  free(buf);

  buf[2] = 99; // <-- write after free

  return 0;
}
