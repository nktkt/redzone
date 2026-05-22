// Heap-buffer-overflow: write one element past a 4-int allocation.
// redzone should catch the store to buf[4] and abort.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int)); // 16 bytes
  if (!buf)
    return 1;

  for (int i = 0; i < 4; i++)
    buf[i] = i;

  buf[4] = 99; // <-- out of bounds: 4 bytes past the end

  printf("%d\n", buf[4]);
  free(buf);
  return 0;
}
