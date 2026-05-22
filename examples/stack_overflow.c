// Expected: stack-buffer-overflow.
// Writes one element past the end of a fixed-size local array. The store to
// a[4] lands in the stack red zone the pass placed after the array, so redzone
// should abort.
#include <stdio.h>

int main(void) {
  int a[4]; // valid indices 0..3, lives on the stack

  for (int i = 0; i < 4; i++)
    a[i] = i;

  a[4] = 99; // <-- out of bounds on the stack

  printf("%d\n", a[4]);
  return 0;
}
