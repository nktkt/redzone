// Expected: OK (clean exit 0).
// Two separate allocations, both used strictly in-bounds and freed once each.
// Verifies redzone tracks multiple live blocks without false positives.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *a = malloc(4 * sizeof(int));
  int *b = malloc(6 * sizeof(int));
  if (!a || !b)
    return 1;

  for (int i = 0; i < 4; i++)
    a[i] = i + 1;
  for (int i = 0; i < 6; i++)
    b[i] = (i + 1) * 10;

  int sum = 0;
  for (int i = 0; i < 4; i++)
    sum += a[i];
  for (int i = 0; i < 6; i++)
    sum += b[i];

  printf("sum = %d\n", sum); // expected 10 + 210 = 220
  free(a);
  free(b);
  return 0;
}
