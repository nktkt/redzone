# Changelog

All notable changes to **redzone** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
[Semantic Versioning](https://semver.org/) once the pass/ABI is frozen at `1.0`.

`v0.18.0` is the first *tagged* release; the `v0.1`–`v0.18` entries below are the
development milestones that led to it (the commit history references them).

## [Unreleased]

### Added
- **sccache** compatibility, alongside ccache: instrumented compiles are
  cacheable and correct, and a versioned plugin filename busts the cache (sccache
  has no `CCACHE_EXTRAFILES`). Verified by `scripts/test_sccache.sh` in CI;
  documented in [`docs/caching.md`](docs/caching.md).

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

[0.19.0]: https://github.com/nktkt/redzone/releases/tag/v0.19.0
[0.18.0]: https://github.com/nktkt/redzone/releases/tag/v0.18.0
