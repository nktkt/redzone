// Expected: heap-buffer-overflow.
// An over-aligned type triggers C++17 aligned operator new; writing one past the
// object lands in the trailing red zone, just like a non-aligned new.
#include <cstdio>

struct alignas(64) Blk {
  char buf[64];
};

int main() {
  Blk *b = new Blk; // aligned operator new (size, align_val_t)
  for (int i = 0; i < 64; i++)
    b->buf[i] = (char)i;

  b->buf[64] = 99; // <-- one past the 64-byte object

  printf("%d\n", b->buf[64]);
  delete b;
  return 0;
}
