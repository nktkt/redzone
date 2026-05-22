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

#ifdef __cplusplus
extern "C" {
#endif

// Drop-in replacements for malloc/free that track allocations and guard them
// with red zones. The pass redirects user malloc/free calls to these.
void *__redzone_malloc(size_t size);
void __redzone_free(void *ptr);

// Validate a memory access of `size` bytes at `addr`. `is_write` is 1 for
// stores, 0 for loads. Aborts with a diagnostic if the access is illegal.
// Addresses that belong to no tracked heap allocation (e.g. stack/global)
// are allowed.
void __redzone_check(const void *addr, size_t size, int is_write);

#ifdef __cplusplus
}
#endif

#endif // REDZONE_RT_H
