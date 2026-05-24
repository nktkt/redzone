// In-bounds memcpy + memset: must run clean (no false positive from the bulk-
// memory interception).
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(16);
  char src[16] = {0};
  volatile size_t n = 16; // exactly fills the allocation
  memcpy(p, src, n);
  memset(p, 0, n);
  free(p);
  return 0;
}
