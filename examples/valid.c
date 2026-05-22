// A correct program: stays within bounds and frees once.
// redzone should let this run to completion without complaint.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int));
  if (!buf)
    return 1;

  int sum = 0;
  for (int i = 0; i < 4; i++)
    buf[i] = i * i;
  for (int i = 0; i < 4; i++)
    sum += buf[i];

  printf("sum = %d\n", sum);
  free(buf);
  return 0;
}
