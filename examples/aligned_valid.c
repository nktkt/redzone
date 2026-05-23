// Expected: clean exit.
// Exercises aligned_alloc and posix_memalign: both must return correctly
// aligned, usable regions that redzone guards like any other heap block.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *p = aligned_alloc(64, 16 * sizeof(int));
  if (!p)
    return 1;
  if (((uintptr_t)p & 63) != 0) // aligned_alloc must honor the alignment
    return 2;
  for (int i = 0; i < 16; i++)
    p[i] = i;

  void *q = NULL;
  if (posix_memalign(&q, 32, 64) != 0)
    return 3;
  if (((uintptr_t)q & 31) != 0) // posix_memalign must honor the alignment
    return 4;
  unsigned char *b = (unsigned char *)q;
  for (int i = 0; i < 64; i++)
    b[i] = (unsigned char)i;

  long sum = 0;
  for (int i = 0; i < 16; i++)
    sum += p[i];
  for (int i = 0; i < 64; i++)
    sum += b[i];

  free(p);
  free(q);
  printf("%ld\n", sum);
  return 0;
}
