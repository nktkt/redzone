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

#include <dlfcn.h>    // dladdr, to locate a frame's module for symbolization
#include <errno.h>
#include <stdarg.h>  // va_list, for the *printf wrappers
#include <execinfo.h> // backtrace / backtrace_symbols for stack traces
#include <pthread.h>  // serialize allocation-table access across threads
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h> // isatty, for color auto-detection

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

// Thread safety. The allocation table is the only shared state that needs a
// lock: record_block reallocs g_blocks (moving it) and bumps g_count, which
// races with concurrent readers. This mutex serializes ALL table access
// (record_block, find_block, block_from_user_ptr, the freed-flag set, leak
// iteration). A Block* obtained under the lock must be used only while the lock
// is held, since a concurrent append may move the array.
//
// The shadow memory needs NO lock: freed blocks are quarantined forever (never
// returned to libc), so a freed address is never re-malloc'd -- shadow writes
// only ever touch fresh or freed memory at disjoint addresses, and distinct
// 16-aligned blocks never share an 8-byte shadow chunk. The inlined per-access
// fast path therefore stays lock-free; only the allocator/error paths lock.
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
#define TABLE_LOCK() pthread_mutex_lock(&g_table_lock)
#define TABLE_UNLOCK() pthread_mutex_unlock(&g_table_lock)

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
// Caller must hold g_table_lock; the returned Block* is valid only under it.
static Block *find_block(uintptr_t addr) {
  for (size_t i = 0; i < g_count; i++) {
    Block *b = &g_blocks[i];
    if (addr >= b->real_base && addr < b->real_base + b->total_size)
      return b;
  }
  return NULL;
}

// Returns the block's index in g_blocks. Caller must hold g_table_lock.
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
// Caller must hold g_table_lock; the returned Block* is valid only under it.
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
// Color
//===----------------------------------------------------------------------===//
//
// ANSI color for text reports. Auto-on when stderr is a TTY; forced by
// REDZONE_COLOR=always|never and disabled by the NO_COLOR convention. col()
// returns the escape code when color is on, or "" otherwise, so call sites
// splice it unconditionally -- a non-TTY (CI log, pipe, test redirect) gets
// clean, code-free text.
#define RZ_RED "\033[1;31m"  // error header
#define RZ_CYAN "\033[36m"   // source locations
#define RZ_DIM "\033[2m"     // secondary detail (alloc site, stack frames)
#define RZ_RESET "\033[0m"

static int rz_color_on(void) {
  static int c = -1;
  if (c < 0) {
    const char *e = getenv("REDZONE_COLOR");
    if (e && strcmp(e, "always") == 0)
      c = 1;
    else if (e && strcmp(e, "never") == 0)
      c = 0;
    else if (getenv("NO_COLOR"))
      c = 0;
    else
      c = isatty(STDERR_FILENO) ? 1 : 0;
  }
  return c;
}

static const char *col(const char *code) { return rz_color_on() ? code : ""; }

//===----------------------------------------------------------------------===//
// Stack traces
//===----------------------------------------------------------------------===//
//
// Captured only on the error/slow path (right before aborting), so they cost
// nothing in steady state. By default frames are symbolized in-process via
// backtrace_symbols (function names + offsets); C++ names stay mangled. Set
// REDZONE_SYMBOLIZE=1 for richer "func (in module) (file:line)" frames, resolved
// best-effort by atos (macOS) / llvm-symbolizer (elsewhere) -- this forks a
// helper per frame on the error path, so it is opt-in, and any frame it can't
// resolve falls back to the backtrace_symbols string.

#define RZ_TRACE_MAX 64

static int rz_symbolize_on(void) {
  static int s = -1;
  if (s < 0) {
    const char *e = getenv("REDZONE_SYMBOLIZE");
    s = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return s;
}

// Best-effort: resolve one frame address to "func (in module) (file:line)" via a
// system symbolizer. Writes into out[outsz] and returns 1 on success, 0 if the
// frame can't be resolved (caller then falls back to the raw frame string).
static int symbolize_frame(void *addr, char *out, size_t outsz) {
  Dl_info info;
  if (!dladdr(addr, &info) || !info.dli_fname || !info.dli_fbase)
    return 0;
  if (strchr(info.dli_fname, '\'')) // a quote would break the shell command
    return 0;
  char cmd[1100];
#if defined(__APPLE__)
  // atos resolves DWARF via the executable's debug map even without a dSYM.
  snprintf(cmd, sizeof cmd, "atos -o '%s' -l %p %p 2>/dev/null", info.dli_fname,
           info.dli_fbase, addr);
#else
  // llvm-symbolizer wants the file-relative (static) address.
  void *rel = (void *)((uintptr_t)addr - (uintptr_t)info.dli_fbase);
  snprintf(cmd, sizeof cmd,
           "llvm-symbolizer --obj='%s' --pretty-print %p 2>/dev/null",
           info.dli_fname, rel);
#endif
  FILE *p = popen(cmd, "r");
  if (!p)
    return 0;
  char buf[512];
  char *got = fgets(buf, sizeof buf, p);
  pclose(p);
  if (!got)
    return 0;
  size_t len = strlen(buf);
  while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    buf[--len] = '\0';
  // Reject useless output: empty, a bare hex address, or an unresolved "??".
  if (len == 0 || strstr(buf, "??") || (buf[0] == '0' && buf[1] == 'x'))
    return 0;
  snprintf(out, outsz, "%s", buf);
  return 1;
}

// Capture the current call stack. Fills `addrs` (caller-provided, RZ_TRACE_MAX
// slots) and returns the backtrace_symbols array (caller frees; may be NULL),
// the frame count in *n, and in *start the index of the first frame outside the
// runtime -- we skip our own frames up to and including the __redzone_* ABI entry
// point the user actually called.
static char **capture_trace(void **addrs, int *n, int *start) {
  int count = backtrace(addrs, RZ_TRACE_MAX);
  char **syms = backtrace_symbols(addrs, count);
  int s = 0;
  if (syms)
    for (int i = 0; i < count; i++)
      if (strstr(syms[i], "__redzone_"))
        s = i + 1; // the first user frame is just past the ABI entry point
  *n = count;
  *start = s;
  return syms;
}

// Strip a leading frame number that some backtrace_symbols implementations
// (e.g. macOS) prefix, so it doesn't collide with our own #N numbering.
static const char *trim_frame(const char *s) {
  while (*s == ' ')
    s++;
  if (*s >= '0' && *s <= '9') {
    while (*s >= '0' && *s <= '9')
      s++;
    while (*s == ' ')
      s++;
  }
  return s;
}

// Append the call stack to a text report, indented and renumbered from #0.
static void print_trace_text(FILE *o) {
  void *addrs[RZ_TRACE_MAX];
  int n, start;
  char **syms = capture_trace(addrs, &n, &start);
  if (!syms)
    return;
  int sym_on = rz_symbolize_on();
  if (start < n)
    fprintf(o, "    %s#stack (most recent call first):%s\n", col(RZ_DIM),
            col(RZ_RESET));
  for (int i = start, f = 0; i < n; i++, f++) {
    char buf[512];
    const char *txt = (sym_on && symbolize_frame(addrs[i], buf, sizeof buf))
                          ? buf
                          : trim_frame(syms[i]);
    fprintf(o, "    %s#%d %s%s\n", col(RZ_DIM), f, txt, col(RZ_RESET));
  }
  free(syms);
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
  int with_stack; // attach the captured call stack (errors, not leaks)?
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

// Append the call stack to a JSON finding as a "stack" array of frame strings.
static void print_trace_json(FILE *o) {
  void *addrs[RZ_TRACE_MAX];
  int n, start;
  char **syms = capture_trace(addrs, &n, &start);
  if (!syms || start >= n) {
    free(syms);
    return;
  }
  int sym_on = rz_symbolize_on();
  fputs(",\"stack\":[", o);
  for (int i = start, f = 0; i < n; i++, f++) {
    char buf[512];
    const char *txt = (sym_on && symbolize_frame(addrs[i], buf, sizeof buf))
                          ? buf
                          : trim_frame(syms[i]);
    if (f)
      fputc(',', o);
    json_str(o, txt);
  }
  fputc(']', o);
  free(syms);
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
  if (fd->with_stack)
    print_trace_json(o);
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
    fd.with_stack = 1;
    emit_finding_and_abort(&fd);
  }
  fprintf(stderr, "%s==redzone ERROR: %s%s of %p\n", col(RZ_RED), kind,
          col(RZ_RESET), ptr);
  print_trace_text(stderr);
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
  TABLE_LOCK();
  size_t idx = record_block((uintptr_t)base, total, user, size, file, line);
  TABLE_UNLOCK();
  // The shadow and header writes below need no lock: this block isn't published
  // to any other thread yet, and its shadow chunks are disjoint from every other
  // block's (see the g_table_lock note).

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
  void *np = __redzone_malloc(size, file, line); // locks internally
  if (!np)
    return NULL;
  // Find how many bytes to carry over, under the lock; don't hold it across the
  // malloc above or the free below (both lock), to avoid re-entrant deadlock.
  TABLE_LOCK();
  Block *old = block_from_user_ptr(ptr);
  if (!old)
    old = find_block((uintptr_t)ptr); // interior pointer: fall back to scanning
  size_t copy = (old && !old->freed)
                    ? (old->user_size < size ? old->user_size : size)
                    : 0;
  TABLE_UNLOCK();
  if (copy)
    memcpy(np, ptr, copy);
  __redzone_free(ptr); // quarantine the old block (locks internally)
  return np;
}

void __redzone_free(void *ptr) {
  if (!ptr)
    return;
  TABLE_LOCK();
  Block *b = block_from_user_ptr(ptr); // O(1): the steady-state path
  if (!b) {
    // Not the start of one of our blocks. Distinguish an interior pointer into
    // one of ours (invalid-free) from a foreign pointer (ignore). This scan is
    // O(n), but it only runs on the error path -- never in steady state.
    b = find_block((uintptr_t)ptr);
    if (!b) {
      TABLE_UNLOCK();
      return; // not one of ours; ignore
    }
    report_free_error("invalid-free", ptr); // aborts (lock held; process dies)
  }
  if (b->freed)
    report_free_error("double-free", ptr); // aborts
  b->freed = 1; // quarantine: keep metadata so use-after-free stays detectable
  // Copy what we need before unlocking, since `b` may move once we release.
  uintptr_t user_addr = b->user_addr;
  size_t user_size = b->user_size;
  TABLE_UNLOCK();
  // Poison the whole user region as freed (shadow needs no lock).
  set_shadow_range(user_addr, (user_size + 7) & ~(size_t)7, FREED_POISON);
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
    fd.with_stack = 1;
    emit_finding_and_abort(&fd);
  }
  uintptr_t a = (uintptr_t)addr;
  void *lo = (void *)b->user_addr;
  void *hi = (void *)(b->user_addr + b->user_size);
  const char *freed = b->freed ? " (freed)" : "";

  fprintf(stderr, "%s==redzone ERROR: %s%s\n", col(RZ_RED), kind, col(RZ_RESET));
  fprintf(stderr, "  %s of size %zu at %p\n", is_write ? "WRITE" : "READ", size,
          addr);
  if (file)
    fprintf(stderr, "    at %s%s:%d%s\n", col(RZ_CYAN), file, line, col(RZ_RESET));
  print_trace_text(stderr);

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
    fprintf(stderr, "    %sallocated at %s:%d%s\n", col(RZ_DIM), b->alloc_file,
            b->alloc_line, col(RZ_RESET));

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
    fd.with_stack = 1;
    emit_finding_and_abort(&fd);
  }
  fprintf(stderr, "%s==redzone ERROR: %s%s\n", col(RZ_RED), kind, col(RZ_RESET));
  fprintf(stderr, "  %s of size %zu at %p\n", is_write ? "WRITE" : "READ", size,
          addr);
  if (file)
    fprintf(stderr, "    at %s%s:%d%s\n", col(RZ_CYAN), file, line, col(RZ_RESET));
  print_trace_text(stderr);
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

  // Slow path: only reached on a violation. Classify it with the table. `b`
  // points into g_blocks, so it (and report, which reads it) must run under the
  // lock; report aborts, so the lock dies with the process.
  TABLE_LOCK();
  Block *b = find_block(a);
  if (b) {
    if (b->freed)
      report("use-after-free", addr, size, is_write, file, line, b);
    else
      report("heap-buffer-overflow", addr, size, is_write, file, line, b);
    TABLE_UNLOCK(); // unreachable (report aborts); keeps the lock balanced
    return;
  }
  TABLE_UNLOCK();
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
// Bulk memory intrinsics (memcpy / memmove / memset).
//
// These are single libc/intrinsic calls, so the per-load/store instrumentation
// never sees the bytes they touch -- an overflow via memcpy would go undetected.
// We bounds-check the whole destination range (and, for copies, the source) by
// reusing __redzone_check, which re-validates the first and last byte (sufficient
// for a contiguous range against the red-zone model) and reports/aborts on a
// violation; a clean range is a no-op. Then we perform the real operation. The
// runtime is compiled without the pass, so these `memcpy`/`memset` calls go
// straight to libc and don't recurse.
//===----------------------------------------------------------------------===//

void *__redzone_memcpy(void *dst, const void *src, size_t n, const char *file,
                       int line) {
  __redzone_check(dst, n, /*is_write=*/1, file, line);
  __redzone_check(src, n, /*is_write=*/0, file, line);
  return memcpy(dst, src, n);
}

void *__redzone_memmove(void *dst, const void *src, size_t n, const char *file,
                        int line) {
  __redzone_check(dst, n, /*is_write=*/1, file, line);
  __redzone_check(src, n, /*is_write=*/0, file, line);
  return memmove(dst, src, n);
}

void *__redzone_memset(void *dst, int c, size_t n, const char *file, int line) {
  __redzone_check(dst, n, /*is_write=*/1, file, line);
  return memset(dst, c, n);
}

//===----------------------------------------------------------------------===//
// String copies (strcpy / strcat / strncpy / strncat).
//
// Same idea as the mem* wrappers, but the access length is implicit, so we
// derive it with strlen/strnlen and bounds-check the resulting ranges. NOTE: we
// must call strlen on the source before checking it; for the common bug (a valid
// source copied into a too-small destination) the source is terminated and this
// is safe, and an over-read of an unterminated source is then itself reported.
//===----------------------------------------------------------------------===//

char *__redzone_strcpy(char *dst, const char *src, const char *file, int line) {
  size_t n = strlen(src) + 1; // bytes read from src and written to dst (incl NUL)
  __redzone_check(src, n, /*is_write=*/0, file, line);
  __redzone_check(dst, n, /*is_write=*/1, file, line);
  return strcpy(dst, src);
}

char *__redzone_strcat(char *dst, const char *src, const char *file, int line) {
  size_t dl = strlen(dst);    // strcat scans dst to its NUL, then appends there
  size_t sl = strlen(src) + 1;
  __redzone_check(src, sl, /*is_write=*/0, file, line);
  __redzone_check(dst, dl + sl, /*is_write=*/1, file, line);
  return strcat(dst, src);
}

char *__redzone_strncpy(char *dst, const char *src, size_t n, const char *file,
                        int line) {
  __redzone_check(dst, n, /*is_write=*/1, file, line); // writes exactly n bytes
  size_t sl = strnlen(src, n);
  __redzone_check(src, (sl < n) ? sl + 1 : n, /*is_write=*/0, file, line);
  return strncpy(dst, src, n);
}

char *__redzone_strncat(char *dst, const char *src, size_t n, const char *file,
                        int line) {
  size_t dl = strlen(dst);
  size_t sl = strnlen(src, n); // appends up to n bytes, then a NUL
  __redzone_check(src, sl, /*is_write=*/0, file, line);
  __redzone_check(dst, dl + sl + 1, /*is_write=*/1, file, line);
  return strncat(dst, src, n);
}

// strlcpy/strlcat (BSD): size-bounded, but an oversized `n` still overflows. We
// check exactly the bytes they actually write -- min(strlen(src)+1, n) for
// strlcpy -- so a large `n` with a short source (which writes little) is not
// flagged, while a genuine overflow is.
size_t __redzone_strlcpy(char *dst, const char *src, size_t n, const char *file,
                         int line) {
  size_t sl = strlen(src);
  __redzone_check(src, sl + 1, /*is_write=*/0, file, line);
  size_t wrote = (n == 0) ? 0 : (sl + 1 <= n ? sl + 1 : n);
  if (wrote)
    __redzone_check(dst, wrote, /*is_write=*/1, file, line);
  return strlcpy(dst, src, n);
}

size_t __redzone_strlcat(char *dst, const char *src, size_t n, const char *file,
                         int line) {
  size_t sl = strlen(src);
  __redzone_check(src, sl + 1, /*is_write=*/0, file, line);
  size_t dl = strnlen(dst, n); // strlcat appends after dst's existing contents
  size_t avail = (dl < n) ? n - dl : 0;
  size_t wrote = (avail == 0) ? 0 : (sl + 1 <= avail ? sl + 1 : avail);
  if (wrote)
    __redzone_check((char *)dst + dl, wrote, /*is_write=*/1, file, line);
  return strlcat(dst, src, n);
}

//===----------------------------------------------------------------------===//
// Formatted output (sprintf / snprintf).
//
// The output length isn't known until the format is expanded, so we measure it
// with vsnprintf(NULL, 0, ...) on a copy of the varargs, bounds-check the bytes
// that will actually be written, then perform the real call. snprintf is bounded
// by `n` (writes min(len+1, n)); sprintf is unbounded (writes len+1).
//===----------------------------------------------------------------------===//

int __redzone_snprintf(char *dst, size_t n, const char *file, int line,
                       const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap); // bytes the format would produce
  va_end(ap);
  if (len >= 0) {
    size_t want = (size_t)len + 1;
    size_t wrote = (n == 0) ? 0 : (want <= n ? want : n);
    if (wrote)
      __redzone_check(dst, wrote, /*is_write=*/1, file, line);
  }
  int r = vsnprintf(dst, n, fmt, ap2);
  va_end(ap2);
  return r;
}

int __redzone_sprintf(char *dst, const char *file, int line, const char *fmt,
                      ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (len >= 0)
    __redzone_check(dst, (size_t)len + 1, /*is_write=*/1, file, line);
  int r = vsnprintf(dst, (size_t)-1, fmt, ap2); // unbounded write, already checked
  va_end(ap2);
  return r;
}

//===----------------------------------------------------------------------===//
// Leak suppressions
//===----------------------------------------------------------------------===//
//
// A leak whose allocation file matches a user-provided substring is silenced.
// Only leaks are suppressible: a hard overflow/use-after-free is a real bug and
// is always reported (continuing past one would be unsafe). REDZONE_SUPPRESSIONS
// names a file of `leak:<substring>` rules (blank lines and `#` comments ignored).

static char **g_supp = NULL;
static size_t g_supp_n = 0;

static void load_suppressions(void) {
  static int loaded = 0;
  if (loaded)
    return;
  loaded = 1;
  const char *path = getenv("REDZONE_SUPPRESSIONS");
  if (!path)
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t')
      s++;
    if (*s == '#' || *s == '\0' || *s == '\n' || *s == '\r')
      continue;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                   s[len - 1] == ' ' || s[len - 1] == '\t'))
      s[--len] = '\0';
    if (strncmp(s, "leak:", 5) != 0)
      continue; // only leak rules are supported for now
    const char *pat = s + 5;
    if (*pat == '\0')
      continue;
    char **g = (char **)realloc(g_supp, (g_supp_n + 1) * sizeof(char *));
    if (!g)
      break;
    g_supp = g;
    g_supp[g_supp_n] = strdup(pat);
    if (g_supp[g_supp_n])
      g_supp_n++;
  }
  fclose(f);
}

static int leak_suppressed(const char *file) {
  if (!file)
    return 0;
  for (size_t i = 0; i < g_supp_n; i++)
    if (strstr(file, g_supp[i]))
      return 1;
  return 0;
}

// A still-live block the user hasn't suppressed -- i.e. a leak we should report.
static int is_unsuppressed_leak(const Block *b) {
  return !b->freed && !leak_suppressed(b->alloc_file);
}

//===----------------------------------------------------------------------===//
// Leak detection (at exit)
//===----------------------------------------------------------------------===//
//
// On a clean exit, any block still in the table that was never freed (and not
// suppressed) is a leak. (Programs that abort via __redzone_check/__redzone_free
// never get here, since abort() bypasses atexit handlers -- so a detected bug is
// never also reported as a leak.) This is a simple "never freed by exit" check;
// reachability-aware leak analysis is a later refinement.

static void report_leaks(void) {
  load_suppressions();
  // Hold the table lock across the whole walk; a thread could still be running.
  // Every exit below either returns (after unlocking) or _Exit()s the process.
  TABLE_LOCK();
  size_t leaked = 0, bytes = 0;
  for (size_t i = 0; i < g_count; i++)
    if (is_unsuppressed_leak(&g_blocks[i])) {
      leaked++;
      bytes += g_blocks[i].user_size;
    }
  if (leaked == 0) {
    TABLE_UNLOCK();
    return;
  }

  rz_format_t fmt = rz_format();
  if (fmt != FMT_TEXT) {
    if (fmt == FMT_JSON) {
      for (size_t i = 0; i < g_count; i++) {
        Block *b = &g_blocks[i];
        if (!is_unsuppressed_leak(b))
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
          if (!is_unsuppressed_leak(b))
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

  fprintf(stderr, "%s==redzone ERROR: memory-leak%s\n", col(RZ_RED),
          col(RZ_RESET));
  fprintf(stderr, "  %zu allocation(s) never freed, %zu byte(s) total\n", leaked,
          bytes);

  // Collapse leaks by allocation site, so a loop that leaks N blocks prints one
  // line (with a count) instead of N. `done` marks blocks already folded into an
  // earlier site's line; if it can't be allocated we fall back to one line each.
  char *done = (char *)calloc(g_count ? g_count : 1, 1);
  for (size_t i = 0; i < g_count; i++) {
    Block *b = &g_blocks[i];
    if (!is_unsuppressed_leak(b) || (done && done[i]))
      continue;
    size_t count = 1, site_bytes = b->user_size;
    if (done)
      for (size_t j = i + 1; j < g_count; j++) {
        Block *o = &g_blocks[j];
        if (done[j] || !is_unsuppressed_leak(o))
          continue;
        int same = o->alloc_line == b->alloc_line &&
                   ((o->alloc_file == b->alloc_file) ||
                    (o->alloc_file && b->alloc_file &&
                     strcmp(o->alloc_file, b->alloc_file) == 0));
        if (same) {
          count++;
          site_bytes += o->user_size;
          done[j] = 1;
        }
      }
    if (b->alloc_file)
      fprintf(stderr, "  %zu allocation(s), %zu byte(s), at %s%s:%d%s\n", count,
              site_bytes, col(RZ_CYAN), b->alloc_file, b->alloc_line,
              col(RZ_RESET));
    else
      fprintf(stderr, "  %zu allocation(s), %zu byte(s) (unknown allocation site)\n",
              count, site_bytes);
  }
  free(done);

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
