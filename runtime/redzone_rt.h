//===- redzone_rt.h - redzone runtime ABI ---------------------------------===//
//
// The contract between the instrumentation pass and the runtime library.
// The pass inserts calls to these functions; this library implements them.
//
// IMPORTANT: compile this runtime WITHOUT the redzone pass, otherwise the
// internal malloc/free here would be rewritten and recurse forever.
//
//===----------------------------------------------------------------------===//
#ifndef REDZONE_RT_H
#define REDZONE_RT_H

#include <stddef.h>

// Mark a function to be excluded from redzone's access checking and stack
// red-zoning -- for a hot path, or code that does intentional out-of-bounds
// pointer math. The function's heap allocations are still tracked (its
// malloc/free are still redirected), so opting out can't corrupt the heap.
// Maps to clang's standard disable_sanitizer_instrumentation attribute.
#if defined(__has_attribute) && __has_attribute(disable_sanitizer_instrumentation)
#define REDZONE_NO_INSTRUMENT __attribute__((disable_sanitizer_instrumentation))
#else
#define REDZONE_NO_INSTRUMENT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Drop-in replacements for malloc/free that track allocations and guard them
// with red zones. The pass redirects user malloc/free calls to these; `file`
// and `line` record the allocation site for diagnostics (may be NULL/0).
void *__redzone_malloc(size_t size, const char *file, int line);
void *__redzone_calloc(size_t nmemb, size_t size, const char *file, int line);
void *__redzone_realloc(void *ptr, size_t size, const char *file, int line);
void __redzone_free(void *ptr);

// aligned_alloc / posix_memalign replacements: same red-zone guarding, but the
// user region is aligned to `alignment`. C++ `new`/`delete` need no new entry
// points -- the pass redirects them to __redzone_malloc/__redzone_free.
void *__redzone_aligned_alloc(size_t alignment, size_t size, const char *file,
                              int line);
int __redzone_posix_memalign(void **memptr, size_t alignment, size_t size,
                             const char *file, int line);

// Validate a memory access of `size` bytes at `addr`. `is_write` is 1 for
// stores, 0 for loads; `file`/`line` locate the access (may be NULL/0). Aborts
// with a diagnostic if the access is illegal. Addresses that belong to no
// tracked heap allocation (e.g. stack/global) are allowed.
void __redzone_check(const void *addr, size_t size, int is_write,
                     const char *file, int line);

// Poison/unpoison the red zones around an enlarged stack allocation. The pass
// calls __redzone_stack_enter at function entry and __redzone_stack_leave
// before each return. `base` points at the enlarged allocation; `user_size` is
// the original variable's size.
void __redzone_stack_enter(void *base, size_t user_size);
void __redzone_stack_leave(void *base, size_t user_size);

// Poison the red zones around a global variable the pass has wrapped. Called
// once per global from a module constructor the pass installs. `data` points at
// the variable's data; `size` is its size. The _register form is for internal
// globals (red zones on both sides); _register_right is for external globals,
// whose data must stay at the symbol's base, so the red zone is trailing-only.
void __redzone_global_register(void *data, size_t size);
void __redzone_global_register_right(void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // REDZONE_RT_H
