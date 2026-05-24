// Heap overflow via strcpy: the source is 6 bytes (incl NUL) but the
// destination holds only 4. redzone intercepts strcpy and bounds-checks the
// derived range.
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(4);
  strcpy(p, "hello"); // 6 > 4 -> writes into the red zone
  free(p);
  return 0;
}
