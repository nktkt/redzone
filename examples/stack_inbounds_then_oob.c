// Expected: stack-buffer-overflow.
// Every index here is a compile-time constant. redzone's selective
// instrumentation skips the checks for the provably in-bounds writes
// (a[0..3]) as an optimization, but it MUST still check and catch the
// out-of-bounds a[4]. This guards against the skip logic suppressing a real
// overflow that sits right next to safe accesses.
#include <stdio.h>

int main(void) {
  int a[4]; // valid indices 0..3

  a[0] = 0;
  a[1] = 1;
  a[2] = 2;
  a[3] = 3;  // all provably in-bounds: their checks are skipped

  a[4] = 99; // <-- constant out-of-bounds: must still be caught

  printf("%d\n", a[3] + a[4]);
  return 0;
}
