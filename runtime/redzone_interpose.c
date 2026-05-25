//===- redzone_interpose.c - whole-process malloc interposition (macOS) ----===//
//
// EXPERIMENTAL but working (scripts/test_interpose.sh). Builds into a dylib that,
// loaded via DYLD_INSERT_LIBRARIES, routes EVERY malloc/calloc/realloc/free in
// the process -- including allocations made inside libraries NOT compiled with
// the redzone pass -- through the redzone runtime, so a prebuilt library's heap
// gets red zones and an instrumented access to it is checked. Complements the
// compile-time call-site redirection; the default mode is unaffected. See
// docs/design/interposition.md.
//
// Three problems, each solved (see the design note):
//   1. Startup introspection (malloc_size/malloc_zone_from_ptr called before any
//      constructor): interpose those queries directly so a redzone block answers
//      for itself.
//   2. Re-entrancy/deadlock (the runtime allocates under its table lock, e.g. for
//      an error backtrace): a per-thread guard (__redzone_in_runtime) makes the
//      interposer forward re-entrant allocations to the system allocator.
//   3. Exit-time false leaks (every live system allocation is a redzone block):
//      the interposer disables the leak report.
//
//   clang -O2 -dynamiclib runtime/redzone_rt.c \
//       -install_name @rpath/libredzone_rt.dylib -o libredzone_rt.dylib
//   clang -O2 -dynamiclib runtime/redzone_interpose.c -L. -lredzone_rt \
//       -Wl,-rpath,. -install_name @rpath/libredzone_interpose.dylib -o ...
//   DYLD_INSERT_LIBRARIES=./libredzone_interpose.dylib ./your_program
//
// The recursion hazard within the runtime's own allocations is independently
// avoided by rz_sys_* allocating through the default malloc zone (a symbol dyld
// does not interpose).
//
//===----------------------------------------------------------------------===//

#include "redzone_rt.h"

#include <malloc/malloc.h> // malloc_zone_t, malloc_zone_register (macOS)
#include <stddef.h>
#include <stdlib.h> // malloc/calloc/realloc/free declarations (interpose targets)

// A dyld interpose tuple: replace every bound reference to `_replacee` with
// `_replacement`, process-wide.
#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
  __attribute__((used)) static struct {                                        \
    const void *replacement;                                                   \
    const void *replacee;                                                      \
  } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {   \
      (const void *)(unsigned long)&_replacement,                              \
      (const void *)(unsigned long)&_replacee}

// Re-entrancy: if this thread is already inside the runtime (holding the table
// lock and, e.g., allocating for an error report), forward to the system
// allocator instead of re-entering redzone -- which would deadlock on the lock.
// For free/realloc the lock-free shadow check inside __redzone_owns is safe to
// run; only the redzone allocate/free paths take the lock.

static void *rz_i_malloc(size_t size) {
  if (__redzone_in_runtime())
    return rz_sys_malloc(size);
  return __redzone_malloc(size, NULL, 0);
}

static void *rz_i_calloc(size_t nmemb, size_t size) {
  if (__redzone_in_runtime()) {
    void *p = rz_sys_malloc(nmemb * size); // overflow-unchecked: re-entrant only
    if (p)
      __builtin_memset(p, 0, nmemb * size);
    return p;
  }
  return __redzone_calloc(nmemb, size, NULL, 0);
}

static void *rz_i_realloc(void *ptr, size_t size) {
  if (__redzone_in_runtime())
    return rz_sys_realloc(ptr, size);
  if (ptr == NULL)
    return __redzone_malloc(size, NULL, 0);
  if (__redzone_owns(ptr))
    return __redzone_realloc(ptr, size, NULL, 0);
  return rz_sys_realloc(ptr, size); // foreign block: grow it in place, untracked
}

static void rz_i_free(void *ptr) {
  if (ptr == NULL)
    return;
  if (__redzone_in_runtime()) {
    // Re-entrant (lock held): use the lock-free check. Re-entrant frees are of
    // system blocks allocated on this same path, so this forwards them; a
    // redzone block here would be a leak rather than a deadlock/corruption,
    // which doesn't occur in practice.
    if (!__redzone_maybe_owns(ptr))
      rz_sys_free(ptr);
    return;
  }
  if (__redzone_owns(ptr))
    __redzone_free(ptr);
  else
    rz_sys_free(ptr); // foreign / pre-init pointer: hand back to the real heap
}

DYLD_INTERPOSE(rz_i_malloc, malloc);
DYLD_INTERPOSE(rz_i_calloc, calloc);
DYLD_INTERPOSE(rz_i_realloc, realloc);
DYLD_INTERPOSE(rz_i_free, free);

//===----------------------------------------------------------------------===//
// Registered malloc zone.
//
// Interposing malloc alone breaks macOS: code (libobjc, libsystem) introspects
// allocations with malloc_size() / malloc_zone_from_ptr(), which walk the
// registered zones asking each "is this pointer yours, and how big?". Our user
// pointers are offset past an in-allocation header, so the *default* zone
// doesn't recognize them and returns size 0 -> "corrupt data pointer" crashes.
// Registering a redzone zone whose `size` callback recognizes our blocks makes
// that introspection succeed.
//===----------------------------------------------------------------------===//

static size_t rz_zone_size(malloc_zone_t *zone, const void *ptr) {
  (void)zone;
  return __redzone_usable_size(ptr); // nonzero iff this is a redzone block
}
static void *rz_zone_malloc(malloc_zone_t *z, size_t s) {
  (void)z;
  return __redzone_malloc(s, NULL, 0);
}
static void *rz_zone_calloc(malloc_zone_t *z, size_t n, size_t s) {
  (void)z;
  return __redzone_calloc(n, s, NULL, 0);
}
static void *rz_zone_valloc(malloc_zone_t *z, size_t s) {
  (void)z;
  return __redzone_malloc(s, NULL, 0);
}
static void *rz_zone_realloc(malloc_zone_t *z, void *p, size_t s) {
  (void)z;
  return rz_i_realloc(p, s);
}
static void rz_zone_free(malloc_zone_t *z, void *p) {
  (void)z;
  rz_i_free(p);
}
static void rz_zone_destroy(malloc_zone_t *z) { (void)z; }
static size_t rz_zone_good_size(malloc_zone_t *z, size_t s) {
  (void)z;
  return s;
}

static boolean_t rz_intro_check(malloc_zone_t *z) {
  (void)z;
  return 1;
}
static void rz_intro_print(malloc_zone_t *z, boolean_t v) {
  (void)z;
  (void)v;
}
static void rz_intro_log(malloc_zone_t *z, void *a) {
  (void)z;
  (void)a;
}
static void rz_intro_noop(malloc_zone_t *z) { (void)z; }
static void rz_intro_stats(malloc_zone_t *z, malloc_statistics_t *s) {
  (void)z;
  if (s)
    *s = (malloc_statistics_t){0};
}
static boolean_t rz_intro_locked(malloc_zone_t *z) {
  (void)z;
  return 0;
}

static struct malloc_introspection_t rz_introspect = {
    .good_size = rz_zone_good_size,
    .check = rz_intro_check,
    .print = rz_intro_print,
    .log = rz_intro_log,
    .force_lock = rz_intro_noop,
    .force_unlock = rz_intro_noop,
    .statistics = rz_intro_stats,
    .zone_locked = rz_intro_locked,
};

static malloc_zone_t rz_zone = {
    .size = rz_zone_size,
    .malloc = rz_zone_malloc,
    .calloc = rz_zone_calloc,
    .valloc = rz_zone_valloc,
    .free = rz_zone_free,
    .realloc = rz_zone_realloc,
    .destroy = rz_zone_destroy,
    .zone_name = "redzone",
    .introspect = &rz_introspect,
    .version = 0,
};

//===----------------------------------------------------------------------===//
// Allocation introspection (the crux of surviving macOS startup).
//
// libobjc / CoreFoundation ask malloc_size() / malloc_zone_from_ptr() how big a
// pointer is and which zone owns it -- during very early image load, before any
// constructor runs. Our user pointers are offset past an in-allocation header,
// so the default zone reports size 0 -> "corrupt data pointer" aborts. We
// interpose the queries themselves (interposes are active from load, not from a
// constructor), so a redzone block answers for itself; foreign pointers fall
// back to the real default zone.
//===----------------------------------------------------------------------===//

static size_t rz_i_malloc_size(const void *ptr) {
  if (ptr && __redzone_owns(ptr))
    return __redzone_usable_size(ptr);
  if (!ptr)
    return 0;
  malloc_zone_t *z = malloc_default_zone(); // not interposed; the real size fn
  return z->size(z, ptr);
}

static malloc_zone_t *rz_i_zone_from_ptr(const void *ptr) {
  if (ptr && __redzone_owns(ptr))
    return &rz_zone; // our zone's callbacks handle size/free for our blocks
  return malloc_zone_from_ptr(ptr);
}

DYLD_INTERPOSE(rz_i_malloc_size, malloc_size);
DYLD_INTERPOSE(rz_i_zone_from_ptr, malloc_zone_from_ptr);

__attribute__((constructor(0))) static void rz_interpose_init(void) {
  malloc_zone_register(&rz_zone);
  // Under whole-process interposition every live process allocation is a redzone
  // block at exit; the exit-time leak report would flag them all, so disable it.
  __redzone_set_leak_report(0);
}
