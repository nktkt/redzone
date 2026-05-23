// Expected: heap-buffer-overflow.
// new[] yields a 4-int region; writing a[4] lands in the trailing red zone.
#include <cstdio>

int main() {
  int *a = new int[4];

  for (int i = 0; i < 4; i++)
    a[i] = i;

  a[4] = 99; // <-- one past the end of a new[] region

  printf("%d\n", a[4]);
  delete[] a;
  return 0;
}
