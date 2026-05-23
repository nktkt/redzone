//===- redzone_race_rt.h - real-execution glue for the race detector ------===//
//
// Horizon 5; see docs/design/data-race-detection.md (phasing step 1b).
//
// This drives the deterministic happens-before state machine in redzone_race.h
// from REAL execution: it owns a process-global detector state, gives every OS
// thread its own logical clock via TLS, and turns synchronization and memory
// operations into happens-before events.
//
// What is here vs. not:
//   - Here: TLS per-thread clocks; a global detector serialized by one lock; a
//     mutex-address -> sync-object registry; thread create/join wrappers that
//     build the parent<->child ordering edges; per-access checks.
//   - NOT here yet: automatic interception. The pass (or a dyld interposer) will
//     eventually call these entry points around every load/store and every
//     pthread call. For now callers invoke the wrappers explicitly -- which is
//     exactly what the unit test does, so the glue is testable WITHOUT relying on
//     any interposition. A normal redzone build does not link this file.
//
// Soundness: a single global lock serializes all detector bookkeeping, so the
// detector's own state is race-free and its decisions are deterministic. The
// happens-before relation is timing-independent, so a correctly-synchronized
// program reports zero races on every run and a genuinely racy one reports at
// least one -- which is what makes a real-thread test deterministic.
//
//===----------------------------------------------------------------------===//
#ifndef REDZONE_RACE_RT_H
#define REDZONE_RACE_RT_H

#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the detector (idempotent; safe to call from any thread). Registers
// the calling thread as the root thread on first call. Call from main before
// spawning, or just let the wrappers below call it for you.
void rz_rt_init(void);

// Thread lifecycle wrappers. Same signatures as pthread_create/join; they add
// the create/join happens-before edges and install the child's logical clock in
// its TLS before its start routine runs.
int rz_rt_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*start_routine)(void *), void *arg);
int rz_rt_pthread_join(pthread_t thread, void **retval);

// Synchronization events, keyed by the lock object's address. These are the
// lower-level record-only primitives; call them alongside your own real locking
// (this is what the deterministic unit test does).
void rz_rt_mutex_lock(void *mutex);   // acquire: import the lock's clock
void rz_rt_mutex_unlock(void *mutex); // release: publish this thread's clock

// pthread_mutex wrappers the pass redirects to: they perform the REAL lock /
// unlock and also record the happens-before event. Same signatures as
// pthread_mutex_lock/unlock so the redirect is a drop-in replacement.
int rz_rt_pthread_mutex_lock(pthread_mutex_t *mutex);
int rz_rt_pthread_mutex_unlock(pthread_mutex_t *mutex);
// trylock records the acquire ONLY when it actually got the lock (returns 0).
int rz_rt_pthread_mutex_trylock(pthread_mutex_t *mutex);

// rwlock wrappers. A read- or write-lock is modeled as an acquire and an unlock
// as a release on the same sync object (keyed by the lock address). This
// over-approximates ordering between concurrent readers -- which is harmless,
// since read/read is never a race -- while capturing every real reader<->writer
// and writer<->writer edge, so it stays free of false positives. try-variants
// record only on success.
int rz_rt_pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int rz_rt_pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int rz_rt_pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int rz_rt_pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
int rz_rt_pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

// Memory access events emitted (eventually) by the instrumentation pass. `size`
// may span several 8-byte words; each is checked.
void rz_rt_read(const volatile void *addr, size_t size);
void rz_rt_write(const volatile void *addr, size_t size);

// Total number of races reported so far (for tests and summaries).
unsigned long rz_rt_race_count(void);

#ifdef __cplusplus
}
#endif

#endif // REDZONE_RACE_RT_H
