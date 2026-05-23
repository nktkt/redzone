// Expected: heap-buffer-overflow.
// aligned_alloc returns a 4-int region; writing a[4] lands in the trailing red
// zone, so redzone should abort just as it does for a malloc'd region.
#include <stdlib.h>

int main(void) {
  int *a = aligned_alloc(64, 4 * sizeof(int));
  if (!a)
    return 1;

  for (int i = 0; i < 4; i++)
    a[i] = i;

  a[4] = 99; // <-- one past the end of an aligned allocation

  free(a);
  return 0;
}
