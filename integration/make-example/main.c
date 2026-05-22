// A deliberately buggy program, used to demonstrate redzone via a plain Makefile.
//
// Expected outcome when built with redzone instrumentation and run:
//   * stderr contains: ==redzone ERROR: heap-buffer-overflow
//   * the process aborts and exits with a NONZERO status.
//
// Build & run from the repo root:
//   make -C integration/make-example
//   ./integration/make-example/demo
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int *buf = malloc(4 * sizeof(int)); // 16 bytes
  if (!buf)
    return 1;

  for (int i = 0; i < 4; i++)
    buf[i] = i;

  buf[4] = 99; // <-- out of bounds: one int past the end of the allocation

  printf("%d\n", buf[4]);
  free(buf);
  return 0;
}
