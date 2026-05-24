// Heap overflow via strncpy: it writes exactly n bytes, and n exceeds the
// destination size.
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(4);
  strncpy(p, "abcdefgh", 8); // writes 8 > 4
  free(p);
  return 0;
}
