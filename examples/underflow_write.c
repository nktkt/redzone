// Expected: heap-buffer-overflow (WRITE, before the region).
// Writes to buf[-1], which lands in the leading red zone just before the
// user region, so redzone should report an access "before" the allocation.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int)); // valid indices 0..3
  if (!buf)
    return 1;

  buf[-1] = 7; // <-- out of bounds: 4 bytes before the start

  printf("%d\n", buf[0]);
  free(buf);
  return 0;
}
