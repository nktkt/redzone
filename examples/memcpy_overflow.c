// Heap overflow via memcpy: the per-load/store checks can't see a single libc
// memcpy, so redzone intercepts memcpy itself and bounds-checks the range.
// A volatile size keeps the length out of the compiler's constant folder (so it
// stays an llvm.memcpy intrinsic rather than a fortified __memcpy_chk).
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(8);
  char src[16] = {0};
  volatile size_t n = 16; // 16 > 8 -> writes into the right red zone
  memcpy(p, src, n);
  free(p);
  return 0;
}
