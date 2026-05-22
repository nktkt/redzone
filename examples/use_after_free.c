// Use-after-free: read from memory after it has been freed.
// redzone should catch the load from buf[0] after free and abort.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int));
  if (!buf)
    return 1;

  buf[0] = 42;
  free(buf);

  printf("%d\n", buf[0]); // <-- use after free
  return 0;
}
