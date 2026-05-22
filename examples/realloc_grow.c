// Expected: OK (clean exit 0).
// Grows an allocation with realloc and uses the larger region in-bounds. Also
// checks that realloc preserved the original contents.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(2 * sizeof(int));
  if (!buf)
    return 1;
  buf[0] = 1;
  buf[1] = 2;

  buf = realloc(buf, 4 * sizeof(int));
  if (!buf)
    return 1;
  buf[2] = 3; // newly grown region, in bounds
  buf[3] = 4;

  int sum = buf[0] + buf[1] + buf[2] + buf[3]; // 10 if the copy was preserved
  printf("sum = %d\n", sum);

  free(buf);
  return 0;
}
