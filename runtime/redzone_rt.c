//===- redzone_rt.c - redzone runtime library -----------------------------===//
//
// Phase 1 (v0.2): heap-buffer-overflow and use-after-free detection.
//
// Strategy (the simple, table-based approach from ROADMAP Horizon 1; shadow
// memory comes later in Horizon 2):
//
//   * Every allocation is padded with a RED ZONE on each side. The real block
//     is [real_base, real_base + total); the user sees [user, user + size).
//   * We record each block in a table.
//   * __redzone_check(addr, size) finds the block whose *full* range contains
//     addr. If addr falls in a red zone (outside the user range) -> overflow.
//     If the block was freed -> use-after-free. Unknown addresses are allowed
//     (they are stack/global accesses we do not track).
//
// Limitations (intentional for the MVP): single-threaded (no locking), freed
// blocks are quarantined and never actually released, and lookup is a linear
// scan. All of these are addressed in later horizons.
//
//===----------------------------------------------------------------------===//

#include "redzone_rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REDZONE_SIZE 16 // guard bytes on each side of an allocation

typedef struct {
  uintptr_t real_base;  // start of the whole allocation, incl. red zones
  size_t total_size;    // full size, incl. both red zones
  uintptr_t user_addr;  // start of the user-visible region
  size_t user_size;     // bytes the caller requested
  int freed;            // quarantined by __redzone_free?
} Block;

static Block *g_blocks = NULL;
static size_t g_count = 0;
static size_t g_cap = 0;

// Return the block whose full range (red zones included) contains `addr`.
static Block *find_block(uintptr_t addr) {
  for (size_t i = 0; i < g_count; i++) {
    Block *b = &g_blocks[i];
    if (addr >= b->real_base && addr < b->real_base + b->total_size)
      return b;
  }
  return NULL;
}

static void record_block(uintptr_t real_base, size_t total_size,
                         uintptr_t user_addr, size_t user_size) {
  if (g_count == g_cap) {
    size_t new_cap = g_cap ? g_cap * 2 : 64;
    Block *n = (Block *)realloc(g_blocks, new_cap * sizeof(Block));
    if (!n) {
      fprintf(stderr, "redzone: out of memory tracking allocations\n");
      abort();
    }
    g_blocks = n;
    g_cap = new_cap;
  }
  Block *b = &g_blocks[g_count++];
  b->real_base = real_base;
  b->total_size = total_size;
  b->user_addr = user_addr;
  b->user_size = user_size;
  b->freed = 0;
}

void *__redzone_malloc(size_t size) {
  size_t total = size + 2 * REDZONE_SIZE;
  unsigned char *base = (unsigned char *)malloc(total);
  if (!base)
    return NULL;
  uintptr_t user = (uintptr_t)base + REDZONE_SIZE;
  record_block((uintptr_t)base, total, user, size);
  return (void *)user;
}

void __redzone_free(void *ptr) {
  if (!ptr)
    return;
  Block *b = find_block((uintptr_t)ptr);
  if (!b)
    return; // not one of ours; ignore
  if ((uintptr_t)ptr != b->user_addr) {
    fprintf(stderr, "==redzone ERROR: invalid-free of %p (not an allocation start)\n",
            ptr);
    abort();
  }
  if (b->freed) {
    fprintf(stderr, "==redzone ERROR: double-free of %p\n", ptr);
    abort();
  }
  b->freed = 1; // quarantine: keep metadata so use-after-free stays detectable
}

static void report(const char *kind, const void *addr, size_t size, int is_write,
                   Block *b) {
  uintptr_t a = (uintptr_t)addr;
  fprintf(stderr, "==redzone ERROR: %s\n", kind);
  fprintf(stderr, "  %s of size %zu at %p\n", is_write ? "WRITE" : "READ", size,
          addr);
  fprintf(stderr, "  region: %zu-byte allocation [%p, %p)%s\n", b->user_size,
          (void *)b->user_addr, (void *)(b->user_addr + b->user_size),
          b->freed ? " (freed)" : "");
  if (a < b->user_addr)
    fprintf(stderr, "  %llu byte(s) before the region\n",
            (unsigned long long)(b->user_addr - a));
  else if (a >= b->user_addr + b->user_size)
    fprintf(stderr, "  %llu byte(s) after the region\n",
            (unsigned long long)(a - (b->user_addr + b->user_size)));
  abort();
}

void __redzone_check(const void *addr, size_t size, int is_write) {
  uintptr_t a = (uintptr_t)addr;
  Block *b = find_block(a);
  if (!b)
    return; // untracked (stack/global) — nothing we can say
  if (b->freed) {
    report("use-after-free", addr, size, is_write, b);
    return;
  }
  if (a >= b->user_addr && a + size <= b->user_addr + b->user_size)
    return; // fully inside the user region: OK
  report("heap-buffer-overflow", addr, size, is_write, b);
}
