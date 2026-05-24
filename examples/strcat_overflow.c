// Heap overflow via strcat: the initial string fits, but appending pushes the
// total past the allocation.
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(4);
  strcpy(p, "ab");  // "ab\0" -> 3 bytes, in bounds
  strcat(p, "cde"); // total "abcde\0" = 6 > 4 -> overflow
  free(p);
  return 0;
}
