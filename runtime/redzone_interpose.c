//===- redzone_interpose.c - whole-process malloc interposition (EXPERIMENTAL) =//
//
// EXPERIMENTAL / NOT PRODUCTION-READY ON macOS. A feasibility spike for routing
// EVERY malloc/calloc/realloc/free in the process -- including allocations made
// inside libraries NOT compiled with the redzone pass -- through the redzone
// runtime, so a prebuilt library's heap gets red zones too. See
// docs/design/interposition.md for the full findings.
//
// What works (established by the spike): dyld's __interpose routes allocations
// to the runtime; the recursion hazard (the runtime's own allocations
// re-entering the interposed malloc) is solved by allocating through the default
// malloc zone (rz_sys_*), a symbol dyld does not replace; foreign pointers are
// forwarded via __redzone_owns; and a registered malloc zone exposes our blocks'
// sizes to introspection.
//
// What does NOT yet work: on macOS, libobjc/libsystem introspect allocations
// (malloc_size / malloc_zone_from_ptr) during very early image load -- before
// any constructor here can register the zone -- so the earliest redzone blocks
// fail introspection and the process aborts ("corrupt data pointer"). A robust
// implementation must install redzone AS the default zone before objc/libsystem
// init and provide a bootstrap allocator for the earliest allocations, i.e. the
// approach AddressSanitizer takes. That is a substantial, separate sub-project;
// this file is the documented starting point. It is built by nothing in the
// normal test/CI flow.
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

static void *rz_i_malloc(size_t size) { return __redzone_malloc(size, NULL, 0); }

static void *rz_i_calloc(size_t nmemb, size_t size) {
  return __redzone_calloc(nmemb, size, NULL, 0);
}

static void *rz_i_realloc(void *ptr, size_t size) {
  if (ptr == NULL)
    return __redzone_malloc(size, NULL, 0);
  if (__redzone_owns(ptr))
    return __redzone_realloc(ptr, size, NULL, 0);
  return rz_sys_realloc(ptr, size); // foreign block: grow it in place, untracked
}

static void rz_i_free(void *ptr) {
  if (ptr == NULL)
    return;
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

__attribute__((constructor(0))) static void rz_register_zone(void) {
  malloc_zone_register(&rz_zone);
}
