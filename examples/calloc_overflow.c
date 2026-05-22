// Expected: heap-buffer-overflow.
// Allocates with calloc (zero-initialized) and writes one element past the end.
// Verifies calloc is tracked just like malloc.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = calloc(4, sizeof(int)); // 16 bytes, zeroed
  if (!buf)
    return 1;

  buf[4] = 7; // <-- out of bounds

  printf("%d\n", buf[0]);
  free(buf);
  return 0;
}
