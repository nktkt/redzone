// A small program with obvious loads and stores for the Phase 0 pass to find.
//
// For now everything here is valid. In later phases we will deliberately
// introduce a heap-buffer-overflow (e.g. write to buf[4]) and a use-after-free
// so the runtime checks have something to catch.

#include <stdio.h>
#include <stdlib.h>

static int sum_array(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i]; // loads from the array
  return s;
}

int main(void) {
  int *buf = malloc(4 * sizeof(int));
  if (!buf)
    return 1;

  for (int i = 0; i < 4; i++)
    buf[i] = i * i; // stores into the array

  int total = sum_array(buf, 4);
  printf("total = %d\n", total);

  free(buf);
  return 0;
}
