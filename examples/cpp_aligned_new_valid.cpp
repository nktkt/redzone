// Expected: clean exit.
// An over-aligned type triggers C++17 aligned operator new / new[]. These must
// be guarded and tracked like the non-aligned forms: the alignment is honored
// and in-bounds use stays clean. Kept trivial (no destructor) and free of
// iostream/exceptions so it links against the C runtime.
#include <cstdint>
#include <cstdio>

struct alignas(64) Blk {
  char buf[64];
};

int main() {
  Blk *b = new Blk; // aligned operator new
  if ((reinterpret_cast<uintptr_t>(b) & 63) != 0)
    return 2; // alignment must be honored
  for (int i = 0; i < 64; i++)
    b->buf[i] = (char)i;

  Blk *a = new Blk[3]; // aligned operator new[]
  if ((reinterpret_cast<uintptr_t>(a) & 63) != 0)
    return 3;

  long sum = 0;
  for (int i = 0; i < 64; i++)
    sum += b->buf[i];
  for (int k = 0; k < 3; k++)
    for (int i = 0; i < 64; i++) {
      a[k].buf[i] = (char)(i + k);
      sum += a[k].buf[i];
    }

  delete[] a; // aligned operator delete[]
  delete b;   // aligned operator delete
  printf("%ld\n", sum);
  return 0;
}
