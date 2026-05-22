// A deliberately buggy program, used to demonstrate redzone via CMake.
// redzone should report a heap-buffer-overflow at the marked line.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int));
  if (!buf)
    return 1;

  for (int i = 0; i < 4; i++)
    buf[i] = i;

  buf[4] = 99; // out of bounds

  printf("%d\n", buf[4]);
  free(buf);
  return 0;
}
