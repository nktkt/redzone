# Changelog

All notable changes to **redzone** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
[Semantic Versioning](https://semver.org/) once the pass/ABI is frozen at `1.0`.

`v0.18.0` is the first *tagged* release; the `v0.1`–`v0.18` entries below are the
development milestones that led to it (the commit history references them).

## [Unreleased]

_Nothing yet._

## [0.24.0] — 2026-05-24

A correctness improvement found by dog-fooding redzone on real libraries, plus
the validation record. Additive; no behavior change for existing code.

### Added
- **Indirect-allocator coverage**: allocations made through a function pointer —
  the common pluggable-allocator pattern of taking the *address* of `malloc` and
  calling it via a hook table — are now tracked. The pass substitutes a
  malloc-compatible wrapper for an address-taken `malloc`/`calloc`/`realloc` (and
  `free`), so blocks allocated indirectly get red zones like direct ones;
  previously only direct `malloc(...)` call sites were redirected. Found by
  dog-fooding redzone on [cJSON](https://github.com/DaveGamble/cJSON) (whose
  hooks default to `{ malloc, free, realloc }`): redzone now both runs clean on a
  full parse/mutate/serialize workload and catches an injected overflow on
  cJSON's heap. Verified by `examples/indirect_malloc.c`.
- **Real-world validation record** ([`docs/real-world-validation.md`](docs/real-world-validation.md)):
  redzone is dog-fooded on cJSON, inih, and stb_ds — each builds, runs clean on a
  realistic workload, and is confirmed to catch an injected overflow on its heap.

## [0.23.0] — 2026-05-24

Completes the library-function overflow coverage started in `0.22.0` with the
formatted-output functions, and adds differential testing against
AddressSanitizer. Additive; no behavior change for in-bounds code.

### Added
- **Formatted-output bounds checking**: `sprintf` and `snprintf` (and their
  fortified `__sprintf_chk` / `__snprintf_chk` forms) are intercepted. Because the
  output length isn't known until the format expands, the wrapper measures it with
  `vsnprintf(NULL, 0, ...)` on a copy of the varargs, bounds-checks the bytes that
  will actually be written (`len+1` for `sprintf`; `min(len+1, n)` for
  `snprintf`), then performs the real write. An overflow such as `sprintf` into a
  too-small buffer is now caught. Verified by `examples/sprintf_overflow.c`,
  `examples/snprintf_overflow.c`, and `examples/printf_valid.c`. (The `va_list`
  variants `vsprintf`/`vsnprintf` are not intercepted.)
- **Differential testing against AddressSanitizer** in CI
  (`scripts/test_diff_asan.sh`): a curated subset of the corpus is built with
  `-fsanitize=address` and ASan must reach the same verdict redzone does — they
  agree on every case. (It also showed redzone catching a `strlcpy` overflow that
  macOS ASan misses, since ASan has no `strlcpy` interceptor there.)
- A **getting-started [tutorial](docs/tutorial.md)**, linked from the README.

## [0.22.0] — 2026-05-24

Closes the bulk- and string-copy overflow gaps in the memory-safety checker:
out-of-bounds writes through `mem*`/`str*` calls — which the per-access
instrumentation can't see (each is one opaque call) — are now detected. No
behavior change for existing in-bounds code; the pass/runtime ABI is additive.

### Added
- **String-copy bounds checking**: `strcpy`, `strcat`, `strncpy`, `strncat`,
  `strlcpy`, and `strlcat` (and their fortified `__*_chk` forms) are intercepted;
  the wrapper derives the access length with `strlen`/`strnlen` and bounds-checks
  the destination (and source) range before the real copy. An overflow such as
  `strcpy` of a too-long string into a small buffer — or `strlcpy` with an
  oversized size argument — is now caught and classified. The `strl*` checks count
  only the bytes actually written, so a large size with a short source isn't
  flagged. Verified by `examples/str{cpy,cat,ncpy,lcpy}_overflow.c` and
  `examples/str_valid.c`.
- **Bulk-memory bounds checking**: `memcpy`, `memmove`, and `memset` — including
  the `llvm.mem*` intrinsics the compiler lowers them to and the fortified
  `__memcpy_chk` / `__memmove_chk` / `__memset_chk` forms — are now intercepted,
  and their destination range (and, for copies, the source range) is bounds-
  checked against the shadow before the real operation runs. Previously an
  overflow through one of these single opaque calls (e.g. `memcpy` into a
  too-small `malloc`) went undetected; now it is caught and classified
  (heap/stack/global overflow, or use-after-free) like any other access. Verified
  by `examples/memcpy_overflow.c`, `examples/memset_overflow.c`, and
  `examples/memcpy_valid.c`.

## [0.21.0] — 2026-05-24

Adds an experimental **data-race detector** alongside the existing memory-safety
checker, plus the last allocator-coverage gap. The default (memory-safety) mode
and its ABI are unchanged; race detection is a separate, opt-in mode.

### Added
- **Experimental data-race detection** (opt-in `--race` mode): a happens-before
  (vector-clock) detector — the model ThreadSanitizer uses — that finds **data
  races**: two accesses to the same location from different threads, at least one
  a write, with no synchronization ordering them. It is a separate, heavier mode
  with its own runtime (`runtime/redzone_race*.c`); the memory-safety checker is
  unaffected. Run it with `scripts/redzone run --race <src.c>`; a race prints both
  conflicting accesses with `file:line` and the process exits nonzero. It models
  the happens-before edges from `pthread_create`/`join`, mutexes (including
  `trylock`), reader/writer locks, condition variables (`pthread_cond_wait`/
  `timedwait`), and C/C++ **atomics** (loads/stores, `atomicrmw`, `cmpxchg`),
  designed around a strict **no-false-positives** rule — unmodeled orderings are
  over-approximated, which can only ever miss a race, never invent one. Validated
  end-to-end (`scripts/test_race_e2e.sh`), with a real-thread runtime test
  (`scripts/test_race_runtime.sh`), and a deterministic engine unit test
  (`scripts/test_race_engine.sh`), all in CI. See the README "Data-race
  detection" section and
  [`docs/design/data-race-detection.md`](docs/design/data-race-detection.md).
  *Limitations:* barriers, semaphores, `pthread_once`, and standalone fences
  (`atomic_thread_fence`) are not yet modeled.
- **C++17 aligned `new`/`delete`** coverage: the over-aligned `operator new` /
  `new[]` (`size, std::align_val_t`) are redirected to the aligned allocator, and
  the aligned `operator delete` / `delete[]` forms (plain and sized) to the
  runtime's free. Over-aligned heap objects now get red zones and tracking like
  any other allocation. Verified by `examples/cpp_aligned_new_{valid,overflow}.cpp`.

## [0.20.0] — 2026-05-23

Caching and exclusion additions on top of `0.19.0`; no runtime ABI or pass
behavior changes for existing users. All changes are exercised in CI.

### Added
- **sccache** compatibility, alongside ccache: instrumented compiles are
  cacheable and correct, and a versioned plugin filename busts the cache (sccache
  has no `CCACHE_EXTRAFILES`). Verified by `scripts/test_sccache.sh` in CI;
  documented in [`docs/caching.md`](docs/caching.md).
- **File-level ignore-list**: `REDZONE_IGNORELIST` names a file of `fun:<glob>` /
  `src:<glob>` rules that exclude matching functions / source files from access
  checking — for third-party code you can't annotate. Same safe semantics as the
  per-function attribute (allocator calls stay redirected). Verified by
  `scripts/test_ignorelist.sh` in CI.

## [0.19.0] — 2026-05-23

Adoption- and scale-focused additions on top of `0.18.0`; no runtime ABI or pass
behavior changes for existing users. All changes are exercised in CI.

### Added
- Opt-in `file:line` in stack-trace frames: `REDZONE_SYMBOLIZE=1` resolves each
  frame to `func (in module) (file:line)` best-effort via `atos` (macOS) /
  `llvm-symbolizer` (elsewhere), falling back to function+offset per frame. Off by
  default, so the default trace stays fast and dependency-free.
- **Caching & reproducible builds** ([`docs/caching.md`](docs/caching.md)):
  instrumented output is byte-identical across builds (verified by
  `scripts/test_determinism.sh`), and redzone works with **ccache** — list the
  plugin in `CCACHE_EXTRAFILES` so a plugin rebuild busts the cache
  (`scripts/test_ccache.sh`). Both run in CI.
- **Per-function opt-out**: `REDZONE_NO_INSTRUMENT` (clang's
  `disable_sanitizer_instrumentation`) excludes a function from access checking
  and stack red-zoning while still redirecting its allocator calls
  (`scripts/test_optout.sh`).

## [0.18.0] — 2026-05-23

The first tagged release. redzone is a minimal, educational AddressSanitizer-style
memory-safety detector for C and C++: an LLVM instrumentation pass plus a small
runtime. It detects heap/stack/global buffer overflow, use-after-free,
double/invalid-free, and memory leaks; runs at roughly AddressSanitizer-level
overhead; and ships with a CLI, machine-readable output, build-system
integration, and CI.

### Reports (v0.18)
- **Colorized** text reports (TTY-aware; honors `NO_COLOR`; `REDZONE_COLOR=always|never`).
- **Leak deduplication**: leaks from the same allocation site collapse into one
  counted line in the text report.

### Earlier milestones

- **v0.17 — Thread safety.** The runtime is thread-safe: an allocation-table
  mutex with a lock-free per-access fast path, so it runs correctly under
  multithreading with no per-access regression.
- **v0.16 — Stack traces & suppressions.** Symbolized call stacks on every error
  (text and JSON); leak suppressions via `REDZONE_SUPPRESSIONS`.
- **v0.15 — External globals.** Overflow detection extended to non-static globals
  with an ABI-safe trailing red zone (cross-TU tested).
- **v0.14 — More allocators.** `aligned_alloc`/`posix_memalign` and C++
  `new`/`new[]`/`delete`/`delete[]`.
- **v0.13 — Selective instrumentation.** The pass skips provably-safe accesses
  (in-bounds of a local, redundant rechecks); ~80–90% fewer checks.
- **v0.12 — O(1) allocator metadata.** Each block finds its metadata via a header
  in its own red zone instead of an O(n) table scan, removing an O(N²) blow-up.
- **v0.11 — Inlined fast path.** The common-case check is emitted inline (shadow
  load + compare + branch); the runtime call is taken only on a flagged byte.
- **v0.10 — Direct-mapped shadow.** Replaced the hashed shadow with a single
  direct-mapped region.
- **v0.9 — Global buffer overflow** (static/internal globals).
- **v0.8 — Machine-readable output.** `REDZONE_FORMAT=json|sarif` (JSON Lines and
  SARIF 2.1.0), with a SARIF → GitHub code-scanning guide.
- **v0.7 — calloc/realloc** coverage.
- **v0.6 — Stack buffer overflow** (the pass wraps static stack allocations).
- **v0.5 — Memory-leak detection** at exit.
- **v0.4 — Shadow memory.** The per-access check became O(1).
- **v0.3 — Readable reports.** Faulting `file:line` and allocation site via debug
  info.
- **v0.2 — Heap buffer overflow & use-after-free.** `malloc`/`free` wrapper, red
  zones, metadata table, free quarantine.
- **v0.1 — Pass skeleton.** An LLVM pass that walks every load/store.

### Developer experience & infrastructure
- `redzone` CLI (`build`/`run`); `demo.sh`.
- Text/JSON/SARIF output; SARIF → code-scanning recipe.
- CMake (`cmake/Redzone.cmake`) and Make integration, with runnable examples and
  a build-integration guide.
- GitHub Actions CI: the test suite, machine-readable-format checks, cross-TU and
  report tests, build-system integration, and a **performance-regression gate**
  (`bench.sh --check`).
- Benchmark harness (`bench/`, `scripts/bench.sh`, `docs/benchmarks.md`).

### Performance
Compute-bound overhead ~1.1×, allocation-heavy ~7.5× (down from ~800× before the
Horizon 4 work); the per-access check is inlined over a direct-mapped shadow. See
[`docs/benchmarks.md`](docs/benchmarks.md).

### Known limitations
Detecting data races; C++17 aligned `new`/`delete`; underflow of an external
global; `file:line` inside stack-trace frames (offline symbolization works today).

[0.20.0]: https://github.com/nktkt/redzone/releases/tag/v0.20.0
[0.19.0]: https://github.com/nktkt/redzone/releases/tag/v0.19.0
[0.18.0]: https://github.com/nktkt/redzone/releases/tag/v0.18.0
