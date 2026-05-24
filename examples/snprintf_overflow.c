// Heap overflow via snprintf with a size larger than the destination: it writes
// min(len+1, n) bytes, which here is 6 > 4.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  char *p = malloc(4);
  snprintf(p, 8, "%s", "hello"); // n=8 > 4; writes 6 bytes
  free(p);
  return 0;
}
