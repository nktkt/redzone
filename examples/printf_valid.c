// In-bounds formatted output: must run clean. snprintf truncates safely within
// its size, and the sprintf output fits the allocation.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  char *p = malloc(16);
  snprintf(p, 16, "%s-%d", "ab", 7); // "ab-7" -> 5 bytes, in bounds
  sprintf(p, "%d", 12345);           // "12345" -> 6 bytes, in bounds
  free(p);
  return 0;
}
