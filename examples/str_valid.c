// In-bounds string copies: must run clean (no false positive from the string
// interception). Exercises strcpy, strcat, strncpy, and strncat in bounds.
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(16);
  strcpy(p, "hi");      // 3 bytes
  strcat(p, "there");   // -> 8 bytes
  strncpy(p, "abc", 4); // 4 bytes
  strncat(p, "xy", 2);  // -> "abcxy\0", in bounds
  free(p);
  return 0;
}
