// Heap overflow via strlcpy: the size argument exceeds the destination, and the
// source is long enough that strlcpy writes past it. (strlcpy is the "safe" BSD
// copy, but an oversized size argument still overflows.)
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(4);
  strlcpy(p, "hello", 8); // n=8 > 4; writes 6 bytes -> past the allocation
  free(p);
  return 0;
}
