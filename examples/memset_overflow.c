// Heap overflow via memset: redzone intercepts memset and bounds-checks the
// destination range. A volatile size keeps it an llvm.memset intrinsic.
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(8);
  volatile size_t n = 16; // 16 > 8 -> writes past the allocation
  memset(p, 0, n);
  free(p);
  return 0;
}
