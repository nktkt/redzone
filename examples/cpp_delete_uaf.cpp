// Expected: use-after-free.
// Reading through a pointer after delete must be caught, just like reading
// after free: delete redirects to the runtime's quarantining free.
#include <cstdio>

int main() {
  int *p = new int(7);

  delete p;

  int v = *p; // <-- use after delete

  printf("%d\n", v);
  return 0;
}
