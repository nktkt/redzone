// Expected: double-free.
// Frees the same pointer twice. The second free hits a quarantined block,
// so redzone should report a double-free and abort.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(8 * sizeof(int));
  if (!buf)
    return 1;

  for (int i = 0; i < 8; i++)
    buf[i] = i;

  free(buf);
  free(buf); // <-- double free

  return 0;
}
