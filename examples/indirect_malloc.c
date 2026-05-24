// Allocators reached through a function pointer (a common pluggable-allocator
// pattern, e.g. cJSON's `{ malloc, free, realloc }` hooks) must still be
// guarded. redzone substitutes a malloc-compatible wrapper for the *address* of
// malloc, so a block allocated via the pointer is still red-zoned.
#include <stdlib.h>

int main(void) {
  void *(*alloc)(size_t) = malloc; // address taken, not a direct call
  void (*dealloc)(void *) = free;
  char *p = (char *)alloc(8);
  p[12] = 'X'; // overflow on an indirectly-allocated block -> must be caught
  dealloc(p);
  return 0;
}
