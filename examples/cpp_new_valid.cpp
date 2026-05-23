// Expected: clean exit.
// C++ new[]/delete[] and new/delete must be guarded and tracked like
// malloc/free. Every access here is in-bounds and everything is released.
// Kept free of iostream/exceptions so it links against the C runtime.
#include <cstdio>

int main() {
  int *a = new int[8];
  for (int i = 0; i < 8; i++)
    a[i] = i * i;

  int *x = new int;
  *x = 42;

  long sum = *x;
  for (int i = 0; i < 8; i++)
    sum += a[i];

  delete x;
  delete[] a;

  printf("%ld\n", sum);
  return 0;
}
