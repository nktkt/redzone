// Heap overflow via sprintf: the formatted output (6 bytes incl NUL) exceeds the
// 4-byte destination. sprintf is unbounded, so redzone measures the output
// length, bounds-checks it, and catches the overflow.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  char *p = malloc(4);
  sprintf(p, "%s", "hello"); // writes "hello\0" = 6 > 4
  free(p);
  return 0;
}
