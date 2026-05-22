// Expected: invalid-free.
// Frees a pointer into the middle of an allocation rather than its start.
// redzone finds the owning block but sees the pointer is not the allocation
// start, so it should report an invalid-free and abort.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int));
  if (!buf)
    return 1;

  for (int i = 0; i < 4; i++)
    buf[i] = i;

  free(buf + 1); // <-- not the allocation start: invalid free

  return 0;
}
