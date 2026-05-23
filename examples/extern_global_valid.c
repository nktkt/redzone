// Expected: clean exit.
// In-bounds use of non-static (external-linkage) globals must not trip a false
// positive, even though they get a trailing red zone.
#include <stdio.h>

long total = 0;    // external scalar global
int data[8] = {0}; // external array global

int main(void) {
  for (int i = 0; i < 8; i++)
    data[i] = i * i;
  for (int i = 0; i < 8; i++)
    total += data[i];

  printf("%ld\n", total);
  return 0;
}
