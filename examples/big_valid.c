// Expected: OK (clean exit 0).
// Allocates a larger array (1000 ints), fills it and sums it fully in-bounds,
// then frees once. Every access is legal, so redzone must not complain.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const int n = 1000;
  int *buf = malloc(n * sizeof(int));
  if (!buf)
    return 1;

  for (int i = 0; i < n; i++)
    buf[i] = i;

  long sum = 0;
  for (int i = 0; i < n; i++)
    sum += buf[i];

  printf("sum = %ld\n", sum); // expected 499500
  free(buf);
  return 0;
}
