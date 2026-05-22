// Expected: heap-buffer-overflow (READ).
// Reads one element past the end of a 5-int allocation. The load from
// buf[5] lands in the trailing red zone, so redzone should abort.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(5 * sizeof(int)); // valid indices 0..4
  if (!buf)
    return 1;

  for (int i = 0; i < 5; i++)
    buf[i] = i + 1;

  int x = buf[5]; // <-- out of bounds: read 4 bytes past the end
  printf("%d\n", x);

  free(buf);
  return 0;
}
