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
//      allocation site. free()/realloc() reach their block in O(1) via a header
//      stashed in the block's left red zone (an index into this table); the
//      table is scanned linearly only to produce a rich report on a violation,
//      or to classify an interior/foreign pointer -- so the common path never
//      walks it.
//
// IMPORTANT: compile this runtime WITHOUT the redzone pass, or its own
// malloc/free below would be rewritten and recurse forever.
//
// Limitations (intentional MVP): single-threaded, freed blocks are quarantined
// and never released. Addressed in later horizons.
//
//===----------------------------------------------------------------------===//

#include "redzone_rt.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define REDZONE_SIZE 16 // guard bytes on each side of an allocation

// Shadow byte encoding (signed):
//   0        all 8 bytes of the chunk are addressable
//   1..7     only the first k bytes are addressable (partial tail)
//   negative poisoned -- the whole chunk is off-limits
#define RZ_POISON ((int8_t)0xFA)    // heap red zone
#define STACK_RZ ((int8_t)0xF1)     // stack red zone
#define GLOBAL_RZ ((int8_t)0xF9)    // global red zone
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

// O(1) back-reference stashed in each heap block's left red zone, so free() and
// realloc() can recover the block's metadata directly from the user pointer
// instead of scanning g_blocks. We store the *index* (stable across the realloc
// that grows g_blocks), not a Block pointer (which the realloc would dangle).
#define RZ_HEADER_MAGIC ((uintptr_t)0x7265647A6F6E6521ULL) // "redzone!"

typedef struct {
  uintptr_t magic; // RZ_HEADER_MAGIC when this is one of our headers
  size_t index;    // index into g_blocks
} Header;

// The header lives in the left red zone, so the red zone must be big enough.
_Static_assert(REDZONE_SIZE >= sizeof(Header), "red zone too small for header");

// Linear scan over every recorded block. O(n), so it must stay off the hot
// path: it is reached only on the error/slow path (a flagged access in
// __redzone_check, or an interior/foreign pointer passed to free/realloc).
static Block *find_block(uintptr_t addr) {
  for (size_t i = 0; i < g_count; i++) {
    Block *b = &g_blocks[i];
    if (addr >= b->real_base && addr < b->real_base + b->total_size)
      return b;
  }
  return NULL;
}

// Returns the block's index in g_blocks.
static size_t record_block(uintptr_t real_base, size_t total_size,
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
  size_t idx = g_count++;
  Block *b = &g_blocks[idx];
  b->real_base = real_base;
  b->total_size = total_size;
  b->user_addr = user_addr;
  b->user_size = user_size;
  b->freed = 0;
  b->alloc_file = file;
  b->alloc_line = line;
  return idx;
}

//===----------------------------------------------------------------------===//
// Shadow memory (fast path)
//===----------------------------------------------------------------------===//

// Direct-mapped shadow: one shadow byte per aligned 8 app bytes, addressed by
//   shadow_byte(app) = g_shadow + (app >> SHADOW_SCALE)
// over a single large reservation. The OS commits shadow pages lazily on first
// touch (zero-filled == addressable), so only the shadow for memory we actually
// poison costs anything. Addresses at/above SHADOW_LIMIT are treated as
// untracked. A flat map makes the lookup a shift+add+load -- and is what lets
// the check be inlined later (see docs/design/inline-fastpath.md).

#define SHADOW_SCALE 3                              // 8 app bytes : 1 shadow byte
#define SHADOW_LIMIT ((uintptr_t)1 << 47)           // cover the 47-bit user space
#define SHADOW_BYTES (SHADOW_LIMIT >> SHADOW_SCALE) // 16 TiB of shadow

// Base of the direct-mapped shadow: the shadow byte for `app` is at
// __redzone_shadow_base + (app >> SHADOW_SCALE). Exported (not static) so the
// instrumentation pass can load it to inline the fast-path check. Reserved
// eagerly by a high-priority constructor before any instrumented access runs;
// covering the whole 47-bit space means the inline path needs no range guard.
int8_t *__redzone_shadow_base = NULL;

static void shadow_init(void) {
  if (__redzone_shadow_base)
    return;
  void *p = mmap(NULL, SHADOW_BYTES, PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) {
    fprintf(stderr, "redzone: failed to reserve shadow memory\n");
    abort();
  }
  __redzone_shadow_base = (int8_t *)p;
}

static int8_t *shadow_ptr(uintptr_t app) {
  if (!__redzone_shadow_base)
    shadow_init();
  uintptr_t idx = app >> SHADOW_SCALE;
  if (idx >= SHADOW_BYTES)
    return NULL; // beyond the covered range -> untracked
  return &__redzone_shadow_base[idx];
}

static int8_t shadow_load(uintptr_t app) {
  int8_t *p = shadow_ptr(app);
  return p ? *p : 0; // uncommitted/out-of-range == addressable
}

static void set_shadow_byte(uintptr_t app, int8_t val) {
  int8_t *p = shadow_ptr(app);
  if (p)
    *p = val;
}

// Set the shadow for every aligned 8-byte chunk overlapping [start, start+len).
// One shadow byte per chunk, contiguous in the direct-mapped shadow, so this is
// a single memset -- not a per-chunk loop. This runs on the allocator hot path
// (poison/carve on malloc, poison-as-freed on free), so keeping it cheap matters.
static void set_shadow_range(uintptr_t start, size_t len, int8_t val) {
  if (len == 0)
    return;
  if (!__redzone_shadow_base)
    shadow_init();
  uintptr_t lo = (start & ~(uintptr_t)7) >> SHADOW_SCALE;        // first chunk
  uintptr_t hi = ((start + len + 7) & ~(uintptr_t)7) >> SHADOW_SCALE; // past end
  if (lo >= SHADOW_BYTES)
    return; // wholly beyond the covered range -> untracked
  if (hi > SHADOW_BYTES)
    hi = SHADOW_BYTES;
  memset(__redzone_shadow_base + lo, (unsigned char)val, hi - lo);
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

// Recover a block from a user pointer in O(1) via the header we stashed in its
// left red zone. Returns NULL unless `ptr` is exactly the start of one of our
// live (or quarantined) heap blocks -- callers fall back to find_block() to tell
// an interior pointer (invalid-free) from a foreign one.
//
// The header sits in poisoned red-zone memory just below the user region. We
// gate the header read on the shadow (always safe to read) being RZ_POISON
// there, so free() of a foreign pointer never dereferences unmapped memory.
static Block *block_from_user_ptr(void *ptr) {
  uintptr_t u = (uintptr_t)ptr;
  if (u < sizeof(Header))
    return NULL;
  if (shadow_load(u - 8) != RZ_POISON)
    return NULL; // not immediately above one of our heap red zones
  const Header *h = (const Header *)(u - sizeof(Header));
  if (h->magic != RZ_HEADER_MAGIC || h->index >= g_count)
    return NULL;
  Block *b = &g_blocks[h->index];
  return b->user_addr == u ? b : NULL;
}

//===----------------------------------------------------------------------===//
// Output formats (text / json / sarif), selected via the REDZONE_FORMAT env var
//===----------------------------------------------------------------------===//

typedef struct {
  const char *kind;  // error kind / SARIF ruleId
  int is_write;      // 1=write, 0=read, -1=not applicable (leak/free)
  size_t size;       // access size, or leaked bytes
  uintptr_t addr;    // faulting/relevant address (0 if N/A)
  const char *file;  // location file (may be NULL)
  int line;          // location line
  int has_region;    // heap region details present?
  size_t region_size;
  int freed; // region already freed?
  const char *alloc_file;
  int alloc_line;
} Finding;

typedef enum { FMT_TEXT, FMT_JSON, FMT_SARIF } rz_format_t;

static rz_format_t rz_format(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *f = getenv("REDZONE_FORMAT");
    if (f && strcmp(f, "json") == 0)
      cached = FMT_JSON;
    else if (f && strcmp(f, "sarif") == 0)
      cached = FMT_SARIF;
    else
      cached = FMT_TEXT;
  }
  return (rz_format_t)cached;
}

static void json_str(FILE *o, const char *s) {
  fputc('"', o);
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"': fputs("\\\"", o); break;
    case '\\': fputs("\\\\", o); break;
    case '\n': fputs("\\n", o); break;
    case '\r': fputs("\\r", o); break;
    case '\t': fputs("\\t", o); break;
    default:
      if (c < 0x20)
        fprintf(o, "\\u%04x", c);
      else
        fputc((int)c, o);
    }
  }
  fputc('"', o);
}

static void json_finding(FILE *o, const Finding *fd) {
  fputs("{\"tool\":\"redzone\",\"error\":", o);
  json_str(o, fd->kind);
  if (fd->is_write >= 0)
    fprintf(o, ",\"access\":\"%s\"", fd->is_write ? "write" : "read");
  fprintf(o, ",\"size\":%zu", fd->size);
  if (fd->addr)
    fprintf(o, ",\"address\":\"0x%llx\"", (unsigned long long)fd->addr);
  if (fd->file) {
    fputs(",\"location\":{\"file\":", o);
    json_str(o, fd->file);
    fprintf(o, ",\"line\":%d}", fd->line);
  }
  if (fd->has_region) {
    fprintf(o, ",\"region\":{\"size\":%zu", fd->region_size);
    if (fd->freed)
      fputs(",\"freed\":true", o);
    if (fd->alloc_file) {
      fputs(",\"allocated\":{\"file\":", o);
      json_str(o, fd->alloc_file);
      fprintf(o, ",\"line\":%d}", fd->alloc_line);
    }
    fputc('}', o);
  }
  fputc('}', o);
}

static void sarif_doc(FILE *o, const Finding *fds, size_t n) {
  fputs("{\"version\":\"2.1.0\",\"$schema\":"
        "\"https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/"
        "Schemata/sarif-schema-2.1.0.json\",\"runs\":[{\"tool\":{\"driver\":{"
        "\"name\":\"redzone\",\"informationUri\":"
        "\"https://github.com/nktkt/redzone\",\"rules\":[]}},\"results\":[",
        o);
  for (size_t i = 0; i < n; i++) {
    const Finding *fd = &fds[i];
    char msg[256];
    if (fd->is_write >= 0)
      snprintf(msg, sizeof msg, "%s of size %zu (%s)",
               fd->is_write ? "WRITE" : "READ", fd->size, fd->kind);
    else
      snprintf(msg, sizeof msg, "%zu byte(s) (%s)", fd->size, fd->kind);
    if (i)
      fputc(',', o);
    fputs("{\"ruleId\":", o);
    json_str(o, fd->kind);
    fputs(",\"level\":\"error\",\"message\":{\"text\":", o);
    json_str(o, msg);
    fputc('}', o);
    if (fd->file) {
      fputs(",\"locations\":[{\"physicalLocation\":{\"artifactLocation\":{"
            "\"uri\":",
            o);
      json_str(o, fd->file);
      fprintf(o, "},\"region\":{\"startLine\":%d}}}]", fd->line);
    }
    fputc('}', o);
  }
  fputs("]}]}\n", o);
}

// Emit a single finding in the active machine-readable format, then abort.
static void emit_finding_and_abort(const Finding *fd) {
  if (rz_format() == FMT_JSON) {
    json_finding(stderr, fd);
    fputc('\n', stderr);
  } else {
    sarif_doc(stderr, fd, 1);
  }
  abort();
}

// Report a free()-time error (double/invalid free), honoring the format.
static void report_free_error(const char *kind, const void *ptr) {
  if (rz_format() != FMT_TEXT) {
    Finding fd = {0};
    fd.kind = kind;
    fd.is_write = -1;
    fd.addr = (uintptr_t)ptr;
    emit_finding_and_abort(&fd);
  }
  fprintf(stderr, "==redzone ERROR: %s of %p\n", kind, ptr);
  abort();
}

//===----------------------------------------------------------------------===//
// Allocation / deallocation
//===----------------------------------------------------------------------===//

static int is_pow2(size_t x) { return x != 0 && (x & (x - 1)) == 0; }

// The common allocator: returns `size` user bytes whose start is `align`-aligned
// (align must be a power of two, >= REDZONE_SIZE), guarded by red zones on both
// sides, recorded in the table, poisoned in the shadow, with the O(1) lookup
// header stashed just below the user region. malloc and the aligned allocators
// all funnel through here; only `align` and the slack reserved for it differ.
static void *rz_allocate(size_t size, size_t align, const char *file, int line) {
  // Reserve the user bytes, both red zones, and (for over-aligned requests)
  // enough slack to round the user pointer up to `align`.
  size_t slack = align > REDZONE_SIZE ? align : 0;
  if (size > SIZE_MAX - 2 * REDZONE_SIZE - slack)
    return NULL; // size computation would overflow
  size_t total = size + 2 * REDZONE_SIZE + slack;
  unsigned char *base = (unsigned char *)malloc(total);
  if (!base)
    return NULL;

  // First `align`-aligned address that leaves room for a left red zone. With the
  // platform's 16-aligned malloc and align == REDZONE_SIZE this is base+16, i.e.
  // identical to the plain malloc layout.
  uintptr_t user = ((uintptr_t)base + REDZONE_SIZE + (align - 1)) & ~(align - 1);
  size_t idx = record_block((uintptr_t)base, total, user, size, file, line);

  // Poison the whole block, then carve out the addressable user region.
  set_shadow_range((uintptr_t)base, total, RZ_POISON);
  size_t aligned = size & ~(size_t)7;
  size_t rem = size & 7;
  if (aligned)
    set_shadow_range(user, aligned, 0);
  if (rem)
    set_shadow_byte(user + aligned, (int8_t)rem);

  // Stash the O(1) back-reference in the (poisoned) left red zone, right below
  // the user region, so free()/realloc() find the block without scanning.
  Header *h = (Header *)(user - sizeof(Header));
  h->magic = RZ_HEADER_MAGIC;
  h->index = idx;

  return (void *)user;
}

void *__redzone_malloc(size_t size, const char *file, int line) {
  return rz_allocate(size, REDZONE_SIZE, file, line);
}

// aligned_alloc(alignment, size): like malloc but the user pointer is aligned to
// `alignment`. We honor any alignment >= REDZONE_SIZE; smaller (but valid) ones
// are rounded up to REDZONE_SIZE, which still satisfies the request and leaves
// room for the red-zone header.
void *__redzone_aligned_alloc(size_t alignment, size_t size, const char *file,
                              int line) {
  if (!is_pow2(alignment))
    return NULL; // C requires a power-of-two alignment
  if (alignment < REDZONE_SIZE)
    alignment = REDZONE_SIZE;
  return rz_allocate(size, alignment, file, line);
}

// posix_memalign(memptr, alignment, size): stores an `alignment`-aligned region
// in *memptr and returns 0, or an errno value on failure (without touching
// *memptr). alignment must be a power of two and a multiple of sizeof(void*).
int __redzone_posix_memalign(void **memptr, size_t alignment, size_t size,
                             const char *file, int line) {
  if (!memptr)
    return EINVAL;
  if (!is_pow2(alignment) || alignment % sizeof(void *) != 0)
    return EINVAL;
  size_t a = alignment < REDZONE_SIZE ? REDZONE_SIZE : alignment;
  void *p = rz_allocate(size, a, file, line);
  if (!p)
    return ENOMEM;
  *memptr = p;
  return 0;
}

void *__redzone_calloc(size_t nmemb, size_t size, const char *file, int line) {
  if (size != 0 && nmemb > SIZE_MAX / size)
    return NULL; // the multiplication would overflow
  size_t total = nmemb * size;
  void *p = __redzone_malloc(total, file, line);
  if (p)
    memset(p, 0, total); // calloc zero-initializes
  return p;
}

void *__redzone_realloc(void *ptr, size_t size, const char *file, int line) {
  if (ptr == NULL)
    return __redzone_malloc(size, file, line);
  if (size == 0) {
    __redzone_free(ptr);
    return NULL;
  }
  void *np = __redzone_malloc(size, file, line);
  if (!np)
    return NULL;
  Block *old = block_from_user_ptr(ptr);
  if (!old)
    old = find_block((uintptr_t)ptr); // interior pointer: fall back to scanning
  if (old && !old->freed) {
    size_t copy = old->user_size < size ? old->user_size : size;
    memcpy(np, ptr, copy);
  }
  __redzone_free(ptr); // quarantine the old block
  return np;
}

void __redzone_free(void *ptr) {
  if (!ptr)
    return;
  Block *b = block_from_user_ptr(ptr); // O(1): the steady-state path
  if (!b) {
    // Not the start of one of our blocks. Distinguish an interior pointer into
    // one of ours (invalid-free) from a foreign pointer (ignore). This scan is
    // O(n), but it only runs on the error path -- never in steady state.
    b = find_block((uintptr_t)ptr);
    if (!b)
      return; // not one of ours; ignore
    report_free_error("invalid-free", ptr); // ours, but not at the user base
  }
  if (b->freed)
    report_free_error("double-free", ptr);
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
// Global red zones
//===----------------------------------------------------------------------===//
//
// The pass wraps each eligible global in a struct with red zones and installs a
// module constructor that calls one of these once per global at startup. `data`
// points at the variable's data; `size` is its size. The red-zone bytes
// physically belong to the wrapper struct, so poisoning their shadow never
// touches a neighbouring symbol.

// Internal globals are wrapped with red zones on BOTH sides ({leftrz, data,
// rightrz}); `data` is REDZONE_SIZE past the wrapper base. Both over- and
// under-flow are caught.
void __redzone_global_register(void *data_v, size_t size) {
  uintptr_t data = (uintptr_t)data_v;
  uintptr_t base = data - REDZONE_SIZE;
  size_t total = size + 2 * REDZONE_SIZE;
  set_shadow_range(base, total, GLOBAL_RZ);
  size_t aligned = size & ~(size_t)7;
  size_t rem = size & 7;
  if (aligned)
    set_shadow_range(data, aligned, 0);
  if (rem)
    set_shadow_byte(data + aligned, (int8_t)rem);
}

// External globals keep `data` at the symbol's base (offset 0) so the address is
// unchanged and other translation units that reference the symbol stay correct;
// the red zone goes only AFTER the data ({data, rightrz}). We must not poison
// anything before `data` -- that memory belongs to another symbol. Overflow is
// caught; underflow of an external global is not (an accepted limitation).
void __redzone_global_register_right(void *data_v, size_t size) {
  uintptr_t data = (uintptr_t)data_v;
  set_shadow_range(data + size, REDZONE_SIZE, GLOBAL_RZ); // trailing red zone
  size_t aligned = size & ~(size_t)7;
  size_t rem = size & 7;
  if (aligned)
    set_shadow_range(data, aligned, 0); // data region addressable
  if (rem)
    set_shadow_byte(data + aligned, (int8_t)rem); // partial tail (re-asserted)
}

//===----------------------------------------------------------------------===//
// The check
//===----------------------------------------------------------------------===//

static void report(const char *kind, const void *addr, size_t size, int is_write,
                   const char *file, int line, Block *b) {
  if (rz_format() != FMT_TEXT) {
    Finding fd = {0};
    fd.kind = kind;
    fd.is_write = is_write;
    fd.size = size;
    fd.addr = (uintptr_t)addr;
    fd.file = file;
    fd.line = line;
    fd.has_region = 1;
    fd.region_size = b->user_size;
    fd.freed = b->freed;
    fd.alloc_file = b->alloc_file;
    fd.alloc_line = b->alloc_line;
    emit_finding_and_abort(&fd);
  }
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
  if (rz_format() != FMT_TEXT) {
    Finding fd = {0};
    fd.kind = kind;
    fd.is_write = is_write;
    fd.size = size;
    fd.addr = (uintptr_t)addr;
    fd.file = file;
    fd.line = line;
    emit_finding_and_abort(&fd);
  }
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
  // Not a tracked heap block: a poisoned stack or global red zone. Read the
  // poison code (from whichever checked byte carries it) to name it precisely.
  int8_t s1 = shadow_load(a);
  int8_t s2 = shadow_load(a + size - 1);
  int8_t code = (s1 < 0) ? s1 : (s2 < 0 ? s2 : 0);
  const char *kind = (code == GLOBAL_RZ)  ? "global-buffer-overflow"
                     : (code == STACK_RZ) ? "stack-buffer-overflow"
                                          : "buffer-overflow";
  report_simple(kind, addr, size, is_write, file, line);
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

  rz_format_t fmt = rz_format();
  if (fmt != FMT_TEXT) {
    if (fmt == FMT_JSON) {
      for (size_t i = 0; i < g_count; i++) {
        Block *b = &g_blocks[i];
        if (b->freed)
          continue;
        Finding fd = {0};
        fd.kind = "memory-leak";
        fd.is_write = -1;
        fd.size = b->user_size;
        fd.addr = b->user_addr;
        fd.file = b->alloc_file;
        fd.line = b->alloc_line;
        json_finding(stderr, &fd);
        fputc('\n', stderr);
      }
    } else { // one SARIF document with a result per leak
      Finding *arr = (Finding *)calloc(leaked, sizeof(Finding));
      if (arr) {
        size_t k = 0;
        for (size_t i = 0; i < g_count; i++) {
          Block *b = &g_blocks[i];
          if (b->freed)
            continue;
          arr[k].kind = "memory-leak";
          arr[k].is_write = -1;
          arr[k].size = b->user_size;
          arr[k].addr = b->user_addr;
          arr[k].file = b->alloc_file;
          arr[k].line = b->alloc_line;
          k++;
        }
        sarif_doc(stderr, arr, k);
        free(arr);
      }
    }
    fflush(NULL);
    _Exit(1);
  }

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

// Reserve the shadow before anything else runs (priority 101 = earliest user
// constructor slot), so the inlined fast-path check can load
// __redzone_shadow_base unconditionally.
__attribute__((constructor(101))) static void __redzone_shadow_startup(void) {
  shadow_init();
}

__attribute__((constructor)) static void redzone_init(void) {
  atexit(report_leaks);
}
