//===- redzone_rt.c - redzone runtime library -----------------------------===//
//
// Detects heap-buffer-overflow, use-after-free, double-free and invalid-free.
//
// v0.2  table-based detection; v0.3  source locations in reports;
// v0.4 (Horizon 2)  shadow memory: the per-access check is now O(1).
//
// Two structures cooperate:
//
//   1. SHADOW MEMORY (the fast path). Each aligned 8-byte chunk of application
//      memory maps to one shadow byte recording how much of it is addressable
//      (see encoding below). __redzone_check reads the shadow for the first and
//      last byte of an access -- O(1), no scanning. Shadow is stored in a
//      lazily-allocated hash of fixed-size chunks (a portable stand-in for the
//      fixed-offset mmap that production ASan uses; see docs/design).
//
//   2. ALLOCATION TABLE (the slow path). Records each block's size, bounds and
//      allocation site. Consulted only by __redzone_free and, on a violation,
//      to produce a rich report -- so the common path never walks it.
//
// IMPORTANT: compile this runtime WITHOUT the redzone pass, or its own
// malloc/free below would be rewritten and recurse forever.
//
// Limitations (intentional MVP): single-threaded, freed blocks are quarantined
// and never released. Addressed in later horizons.
//
//===----------------------------------------------------------------------===//

#include "redzone_rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REDZONE_SIZE 16 // guard bytes on each side of an allocation

// Shadow byte encoding (signed):
//   0        all 8 bytes of the chunk are addressable
//   1..7     only the first k bytes are addressable (partial tail)
//   negative poisoned -- the whole chunk is off-limits
#define RZ_POISON ((int8_t)0xFA)    // heap red zone
#define STACK_RZ ((int8_t)0xF1)     // stack red zone
#define FREED_POISON ((int8_t)0xFD) // freed memory

//===----------------------------------------------------------------------===//
// Allocation table (slow path: free + reporting)
//===----------------------------------------------------------------------===//

typedef struct {
  uintptr_t real_base;    // start of the whole allocation, incl. red zones
  size_t total_size;      // full size, incl. both red zones
  uintptr_t user_addr;    // start of the user-visible region
  size_t user_size;       // bytes the caller requested
  int freed;              // quarantined by __redzone_free?
  const char *alloc_file; // source file of the allocation (may be NULL)
  int alloc_line;         // source line of the allocation
} Block;

static Block *g_blocks = NULL;
static size_t g_count = 0;
static size_t g_cap = 0;

static Block *find_block(uintptr_t addr) {
  for (size_t i = 0; i < g_count; i++) {
    Block *b = &g_blocks[i];
    if (addr >= b->real_base && addr < b->real_base + b->total_size)
      return b;
  }
  return NULL;
}

static void record_block(uintptr_t real_base, size_t total_size,
                         uintptr_t user_addr, size_t user_size,
                         const char *file, int line) {
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
  b->alloc_file = file;
  b->alloc_line = line;
}

//===----------------------------------------------------------------------===//
// Shadow memory (fast path)
//===----------------------------------------------------------------------===//

#define CHUNK_SHIFT 16                          // each chunk covers 64 KiB
#define CHUNK_APP ((uintptr_t)1 << CHUNK_SHIFT) // app bytes per chunk
#define CHUNK_SHADOW (CHUNK_APP >> 3)           // shadow bytes per chunk (8 KiB)

typedef struct {
  uintptr_t key;   // app_addr >> CHUNK_SHIFT
  int8_t *shadow;  // CHUNK_SHADOW bytes, or NULL if the slot is empty
} ChunkEntry;

static ChunkEntry *g_dir = NULL; // open-addressing hash table of chunks
static size_t g_dir_cap = 0;
static size_t g_dir_count = 0;

static size_t hash_key(uintptr_t key) {
  return (size_t)(key * 0x9E3779B97F4A7C15ull); // Fibonacci hashing
}

static void dir_insert(ChunkEntry *dir, size_t cap, uintptr_t key,
                       int8_t *shadow) {
  size_t mask = cap - 1;
  size_t i = hash_key(key) & mask;
  while (dir[i].shadow)
    i = (i + 1) & mask;
  dir[i].key = key;
  dir[i].shadow = shadow;
}

static void dir_grow(void) {
  size_t new_cap = g_dir_cap ? g_dir_cap * 2 : 1024;
  ChunkEntry *n = (ChunkEntry *)calloc(new_cap, sizeof(ChunkEntry));
  if (!n) {
    fprintf(stderr, "redzone: out of memory growing shadow directory\n");
    abort();
  }
  for (size_t i = 0; i < g_dir_cap; i++)
    if (g_dir[i].shadow)
      dir_insert(n, new_cap, g_dir[i].key, g_dir[i].shadow);
  free(g_dir);
  g_dir = n;
  g_dir_cap = new_cap;
}

// Return the shadow chunk for `key`, allocating it (and the directory) if
// `create` is set. A fresh chunk is all-zero, i.e. fully addressable.
static int8_t *get_chunk(uintptr_t key, int create) {
  if (!g_dir) {
    if (!create)
      return NULL;
    dir_grow();
  }
  if (create && (g_dir_count + 1) * 4 >= g_dir_cap * 3) // keep load factor < 3/4
    dir_grow();

  size_t mask = g_dir_cap - 1;
  size_t i = hash_key(key) & mask;
  while (g_dir[i].shadow) {
    if (g_dir[i].key == key)
      return g_dir[i].shadow;
    i = (i + 1) & mask;
  }
  if (!create)
    return NULL;

  int8_t *chunk = (int8_t *)calloc(CHUNK_SHADOW, 1); // 0 == addressable
  if (!chunk) {
    fprintf(stderr, "redzone: out of memory allocating shadow chunk\n");
    abort();
  }
  g_dir[i].key = key;
  g_dir[i].shadow = chunk;
  g_dir_count++;
  return chunk;
}

static int8_t *shadow_ptr(uintptr_t app, int create) {
  int8_t *chunk = get_chunk(app >> CHUNK_SHIFT, create);
  if (!chunk)
    return NULL;
  return &chunk[(app & (CHUNK_APP - 1)) >> 3];
}

static int8_t shadow_load(uintptr_t app) {
  int8_t *p = shadow_ptr(app, 0);
  return p ? *p : 0; // no chunk == addressable
}

static void set_shadow_byte(uintptr_t app, int8_t val) {
  int8_t *p = shadow_ptr(app, 1);
  *p = val;
}

// Set the shadow for every aligned 8-byte chunk overlapping [start, start+len).
static void set_shadow_range(uintptr_t start, size_t len, int8_t val) {
  uintptr_t a = start & ~(uintptr_t)7;            // round down
  uintptr_t end = (start + len + 7) & ~(uintptr_t)7; // round up
  for (; a < end; a += 8)
    set_shadow_byte(a, val);
}

// Is the byte at `app` off-limits?
static int byte_poisoned(uintptr_t app) {
  int8_t sv = shadow_load(app);
  if (sv == 0)
    return 0; // whole chunk addressable
  if (sv < 0)
    return 1; // poisoned
  return (int)(app & 7) >= sv; // partial: only first `sv` bytes are valid
}

//===----------------------------------------------------------------------===//
// Allocation / deallocation
//===----------------------------------------------------------------------===//

void *__redzone_malloc(size_t size, const char *file, int line) {
  size_t total = size + 2 * REDZONE_SIZE;
  unsigned char *base = (unsigned char *)malloc(total);
  if (!base)
    return NULL;
  uintptr_t user = (uintptr_t)base + REDZONE_SIZE;
  record_block((uintptr_t)base, total, user, size, file, line);

  // Poison the whole block, then carve out the addressable user region.
  set_shadow_range((uintptr_t)base, total, RZ_POISON);
  size_t aligned = size & ~(size_t)7;
  size_t rem = size & 7;
  if (aligned)
    set_shadow_range(user, aligned, 0);
  if (rem)
    set_shadow_byte(user + aligned, (int8_t)rem);

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
  // Poison the whole user region as freed.
  set_shadow_range(b->user_addr, (b->user_size + 7) & ~(size_t)7, FREED_POISON);
}

//===----------------------------------------------------------------------===//
// Stack red zones
//===----------------------------------------------------------------------===//
//
// The pass enlarges each static stack allocation with red zones and calls these
// to poison/unpoison the surrounding shadow. `base` points at the enlarged
// allocation; `user_size` is the original variable's size. No table entry is
// made -- stack lifetimes are scoped, and the report path handles them without
// one. Leaving restores the shadow so the stack slot can be reused.

void __redzone_stack_enter(void *base_v, size_t user_size) {
  uintptr_t base = (uintptr_t)base_v;
  uintptr_t user = base + REDZONE_SIZE;
  size_t total = user_size + 2 * REDZONE_SIZE;
  set_shadow_range(base, total, STACK_RZ);
  size_t aligned = user_size & ~(size_t)7;
  size_t rem = user_size & 7;
  if (aligned)
    set_shadow_range(user, aligned, 0);
  if (rem)
    set_shadow_byte(user + aligned, (int8_t)rem);
}

void __redzone_stack_leave(void *base_v, size_t user_size) {
  uintptr_t base = (uintptr_t)base_v;
  size_t total = user_size + 2 * REDZONE_SIZE;
  set_shadow_range(base, total, 0); // restore: addressable again for reuse
}

//===----------------------------------------------------------------------===//
// The check
//===----------------------------------------------------------------------===//

static void report(const char *kind, const void *addr, size_t size, int is_write,
                   const char *file, int line, Block *b) {
  uintptr_t a = (uintptr_t)addr;
  void *lo = (void *)b->user_addr;
  void *hi = (void *)(b->user_addr + b->user_size);
  const char *freed = b->freed ? " (freed)" : "";

  fprintf(stderr, "==redzone ERROR: %s\n", kind);
  fprintf(stderr, "  %s of size %zu at %p\n", is_write ? "WRITE" : "READ", size,
          addr);
  if (file)
    fprintf(stderr, "    at %s:%d\n", file, line);

  if (a < b->user_addr)
    fprintf(stderr, "  %llu byte(s) before a %zu-byte region [%p, %p)%s\n",
            (unsigned long long)(b->user_addr - a), b->user_size, lo, hi, freed);
  else if (a >= b->user_addr + b->user_size)
    fprintf(stderr, "  %llu byte(s) after a %zu-byte region [%p, %p)%s\n",
            (unsigned long long)(a - (b->user_addr + b->user_size)), b->user_size,
            lo, hi, freed);
  else
    fprintf(stderr, "  inside a %zu-byte region [%p, %p)%s\n", b->user_size, lo,
            hi, freed);

  if (b->alloc_file)
    fprintf(stderr, "    allocated at %s:%d\n", b->alloc_file, b->alloc_line);

  abort();
}

// Report for a non-heap region (stack/global) where we have no table entry.
static void report_simple(const char *kind, const void *addr, size_t size,
                          int is_write, const char *file, int line) {
  fprintf(stderr, "==redzone ERROR: %s\n", kind);
  fprintf(stderr, "  %s of size %zu at %p\n", is_write ? "WRITE" : "READ", size,
          addr);
  if (file)
    fprintf(stderr, "    at %s:%d\n", file, line);
  abort();
}

void __redzone_check(const void *addr, size_t size, int is_write,
                     const char *file, int line) {
  if (size == 0)
    return;
  uintptr_t a = (uintptr_t)addr;

  // Fast path: a poisoned first or last byte means the access is illegal.
  if (!byte_poisoned(a) && !byte_poisoned(a + size - 1))
    return;

  // Slow path: only reached on a violation. Classify it with the table.
  Block *b = find_block(a);
  if (b) {
    if (b->freed)
      report("use-after-free", addr, size, is_write, file, line, b);
    else
      report("heap-buffer-overflow", addr, size, is_write, file, line, b);
    return;
  }
  // Not a tracked heap block: a poisoned stack red zone (globals not yet
  // covered). Report with the faulting location.
  report_simple("stack-buffer-overflow", addr, size, is_write, file, line);
}

//===----------------------------------------------------------------------===//
// Leak detection (at exit)
//===----------------------------------------------------------------------===//
//
// On a clean exit, any block still in the table that was never freed is a leak.
// (Programs that abort via __redzone_check/__redzone_free never get here, since
// abort() bypasses atexit handlers -- so a detected bug is never also reported
// as a leak.) This is a simple "never freed by exit" check; reachability-aware
// leak analysis is a later refinement.

static void report_leaks(void) {
  size_t leaked = 0, bytes = 0;
  for (size_t i = 0; i < g_count; i++)
    if (!g_blocks[i].freed) {
      leaked++;
      bytes += g_blocks[i].user_size;
    }
  if (leaked == 0)
    return;

  fprintf(stderr, "==redzone ERROR: memory-leak\n");
  fprintf(stderr, "  %zu allocation(s) never freed, %zu byte(s) total\n", leaked,
          bytes);
  for (size_t i = 0; i < g_count; i++) {
    Block *b = &g_blocks[i];
    if (b->freed)
      continue;
    if (b->alloc_file)
      fprintf(stderr, "  %zu byte(s) allocated at %s:%d\n", b->user_size,
              b->alloc_file, b->alloc_line);
    else
      fprintf(stderr, "  %zu byte(s) (unknown allocation site)\n", b->user_size);
  }

  fflush(NULL); // flush the program's own output before forcing the exit code
  _Exit(1);     // signal the leak via a nonzero status
}

__attribute__((constructor)) static void redzone_init(void) {
  atexit(report_leaks);
}
