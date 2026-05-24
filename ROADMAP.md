# redzone — Roadmap

A long-range plan to grow `redzone` from an educational heap checker into a
**scalable, production-grade memory-safety platform**.

## Vision

> Make memory-safety checking something that scales from a single developer's
> laptop to an entire organization's CI — fast enough to run on every build,
> simple enough to adopt in minutes, and extensible enough to detect new classes
> of bugs over time.

## Design pillars

These guide every decision. A feature that breaks a pillar waits.

1. **Correctness first** — no false negatives on the bug classes we claim to
   detect; minimize false positives. Trust is the product.
2. **Low overhead** — runtime and memory cost must stay within a budget so the
   tool can run on real workloads, not just toy programs.
3. **Scalability** — works on million-line codebases and thousands of CI runs,
   not just `hello.c`.
4. **Developer experience** — adopt in minutes, reports a human can act on.
5. **Extensibility** — new bug detectors plug into shared infrastructure
   instead of being rewritten from scratch.

---

## Horizon 1 — Foundation (correctness MVP) · `v0.1`–`v0.3`

Prove the core idea end to end on small programs.

- `v0.1` ✅ — **Pass skeleton.** Walk every `load`/`store`; print them. Proves
  the LLVM pass plugs into the compiler.
- `v0.2` ✅ — **Heap-buffer-overflow and use-after-free detection.** `malloc`
  wrapper + red zones + metadata table; `__redzone_check` validates accesses;
  `__redzone_free` quarantines freed blocks.
- `v0.3` ✅ — **Readable reports:** `file:line` of the faulting access and the
  allocation site, via debug info.

**Exit criteria:** a golden corpus of known-bad programs is caught, known-good
programs run clean, all from a single `clang` invocation.

## Horizon 2 — Robustness & performance · `v0.4`–`v0.6`

Make it correct and fast enough to point at real code.

- ✅ **Shadow memory model** — replace the O(n) metadata table with O(1) shadow
  lookups. *This is the central scalability unlock.* (Done in v0.4: lazy hashed
  shadow chunks; table kept for error-path reports only.)
- ✅ **Allocator coverage** — `malloc`/`calloc`/`realloc`/`free` (v0.7),
  `aligned_alloc`/`posix_memalign` and C++ `new`/`new[]`/`delete`/`delete[]`
  (v0.14), including the C++17 aligned `new`/`delete` forms. Complete.
- ✅ **Stack buffer overflow** detection (v0.6): the pass wraps each static
  stack allocation with red zones, poisoned on entry and restored on return.
- ✅ **Global buffer overflow** detection (v0.9, v0.15): the pass wraps eligible
  globals in red-zone-padded structs and poisons them via a module constructor —
  internal globals on both sides, and external (non-static) globals with a
  trailing red zone that preserves the symbol's address for other TUs (v0.15;
  cross-TU tested). External-global underflow remains out of scope.
- ✅ **Memory leak detection** at exit (v0.5): un-freed blocks are reported with
  their allocation site; the process exits nonzero. (Reachability-aware analysis
  is a later refinement.)
- **Performance budget** — target ≤ 2–3× slowdown; track memory overhead.

**Exit criteria:** runs cleanly and usefully on a mid-size open-source C project.

## Horizon 3 — Developer experience & tooling · `v0.7`–`v0.9`

Turn a compiler pass into a product people choose to use.

- ✅ **CLI / compiler wrapper** (`scripts/redzone`) — `build`/`run` subcommands
  that automate emit-IR → instrument → link, and build the plugin on first use.
- ✅ **Great reports** — symbolized **stack traces** on every error in text and
  JSON (v0.16); **colorized** TTY-aware output and **deduplicated** leak summaries
  (v0.18); opt-in `file:line` in trace frames via `REDZONE_SYMBOLIZE` (v0.19,
  best-effort atos/llvm-symbolizer). Deduping repeated *errors* would need a
  continue-after-error mode (we abort on first).
- 🟡 **Suppression files** — `REDZONE_SUPPRESSIONS` silences known **leaks** by
  allocation file (v0.16). Hard errors are intentionally never suppressed.
- ✅ **Machine-readable output** (`REDZONE_FORMAT=json|sarif`) — JSON Lines and
  SARIF 2.1.0, emitted to stderr (v0.8).
- ✅ **Build-system recipes** — CMake (`cmake/Redzone.cmake`) and Make, both via
  `-fpass-plugin`, with runnable examples and a guide (`docs/build-integration.md`),
  exercised in CI. Bazel later.
- ✅ **CI** — GitHub Actions builds the pass and runs the test suite + the
  machine-readable-format checks on every push/PR (macOS + Homebrew LLVM). A
  SARIF→code-scanning recipe is in `docs/ci-integration.md`.
- **Docs site** — tutorials, examples, troubleshooting.

**Exit criteria:** a new user adopts redzone in an existing project in < 15 min.

## Horizon 4 — Scale · `v1.0`+

Run on huge codebases and many builds without pain.

- ✅ **Selective instrumentation** — skip provably-safe accesses via static
  analysis (v0.13: in-bounds-of-alloca + redundant rechecks; ~80–90% fewer
  checks, `docs/design/selective-instrumentation.md`), plus a **per-function
  opt-out** (`REDZONE_NO_INSTRUMENT` /
  `__attribute__((disable_sanitizer_instrumentation))`) and a **file-level
  ignore-list** (`REDZONE_IGNORELIST` with `fun:`/`src:` globs). Cross-block /
  loop-range check elimination remains.
- 🟡 **Incremental builds & caching** — instrumented output is **reproducible**
  (byte-identical; verified in CI), so per-TU incremental rebuilds and compiler
  caches work. **ccache** and **sccache** are both supported and tested (ccache:
  put the plugin in `CCACHE_EXTRAFILES`; sccache: version the plugin filename) —
  see [`docs/caching.md`](docs/caching.md). sccache's shared/distributed backends
  let a fleet share the cache; bespoke distributed-build systems remain.
- **Parallel / distributed test execution.**
- **Cross-platform** — Linux, macOS, Windows; x86-64 and ARM64;
  cross-compilation.
- **Stability guarantees** — semantic versioning, frozen pass-plugin API and
  runtime ABI at `v1.0`.

**Exit criteria:** runs on a multi-million-line codebase in CI within an
acceptable time and cost budget.

## Horizon 5 — Platform & ecosystem · `v2.0` vision

From a tool to a platform.

- **Multi-language** — full C++; explore other LLVM frontends (Rust `unsafe`,
  Swift, …).
- **Checker plugin API** — third parties add new detectors on the shared
  instrumentation + shadow-memory infrastructure (new "modes" alongside
  address/leak: uninitialized memory, undefined behavior, data races).
- 🟡 **Data-race detection** — a happens-before (vector-clock) detector reusing
  the pass's access walk but with its own shadow, runtime, and build mode.
  Designed in
  [`docs/design/data-race-detection.md`](docs/design/data-race-detection.md); the
  happens-before detector now runs **end to end**: the engine + deterministic
  state machine (`runtime/redzone_race.{c,h}`, scenarios A–I), a real-thread
  runtime (`runtime/redzone_race_rt.{c,h}` — TLS clocks, create/join wrappers,
  mutex acquire/release), and a **pass mode** (`-redzone-race`) that auto-emits
  the access hooks and redirects the pthread primitives — `create`/`join`,
  `mutex_lock`/`unlock`/`trylock`, reader/writer locks, and condition variables.
  A racy program is flagged and correctly-synchronized ones (mutex, rwlock,
  trylock, condvar producer/consumer) run clean (`scripts/test_race_e2e.sh`); the
  runtime alone is also stress-tested with real pthreads, 25× in CI. **C/C++
  atomics** are modeled as acquire/release synchronization (loads/stores,
  `atomicrmw`, `cmpxchg`), so lock-free code no longer false-positives. A detected
  race reports both conflicting accesses with thread id and `file:line`,
  deduplicated by source position. Remaining breadth — barriers, semaphores,
  `pthread_once`, precise atomic memory orders and fences, and performance tuning
  — is still ahead; it stays a distinct opt-in mode.
- **Findings dashboard** — aggregate results across builds, track regressions
  and trends over time, triage workflow. *(The org-scale / SaaS angle.)*
- **Fuzzing integration** — coverage-guided fuzzing with redzone as the crash
  oracle.
- **IDE integration** — inline diagnostics (VS Code extension).
- **Cloud offering** — org-wide policy, historical analytics, regression gating.

---

## Cross-cutting tracks (continuous)

Run alongside every horizon, not in sequence.

- 🟡 **Testing & correctness** — golden corpus of bad/good programs (the
  `examples/` suite); track false-positive / false-negative rates. **Differential
  testing against real AddressSanitizer** is wired into CI (`scripts/test_diff_asan.sh`):
  a curated subset is built with `-fsanitize=address` and ASan must reach the same
  verdict redzone does. (It surfaced that redzone catches a `strlcpy` overflow
  macOS ASan misses — ASan has no `strlcpy` interceptor there.)
- ✅ **Benchmarking** — `scripts/bench.sh` measures instrumented-vs-baseline
  overhead (`docs/benchmarks.md`); `--check` mode runs in CI as a regression gate
  (static check-count budgets + loose slowdown ceilings).
- **Release engineering** — CI/CD, prebuilt binaries, packaging (Homebrew, apt).
- **Security & hardening** — the tool itself must be robust against the inputs
  it analyzes. 🟡 The runtime is now **thread-safe** (v0.17): an allocation-table
  mutex with a lock-free per-access fast path, so it runs correctly under
  multithreading (stress-tested). Detecting data races is a separate Horizon 5
  checker.
- **Community** — contribution guide, issue templates, governance, changelog.

## What "scalable" means here

| Dimension | Mechanism |
|---|---|
| **Big codebases** | Shadow memory, selective + incremental instrumentation |
| **Many builds** | Build-cache compatibility, parallel/distributed runs |
| **Many bug classes** | Shared infra + checker plugin API |
| **Many languages** | LLVM IR as the common substrate |
| **Many teams** | Findings dashboard, suppressions, CI gating, policy |

## Success metrics

- **Correctness:** ≥ 99% detection on the golden corpus; near-zero false
  positives on known-good code.
- **Performance:** ≤ 2–3× runtime overhead at `v1.0`.
- **Adoption:** time-to-first-bug-found < 15 min for a new project.
- **Scale:** full CI run on a multi-million-line codebase within budget.

## Now / Next / Later

- **Done:** `v0.1`–`v0.3` (heap-overflow + use-after-free, readable reports,
  test suite); `v0.4` shadow memory (O(1) check); `v0.5` leak detection; `v0.6`
  stack-buffer-overflow. Suite is 12/12.
- **Horizon 3 done:** CLI wrapper; CI (suite + format + integration checks);
  machine-readable output with a SARIF→code-scanning guide; CMake & Make recipes.
- **Now (Horizon 4):** **direct-mapped shadow** (v0.10), **inlined fast-path
  check** (v0.11), **O(1) allocator metadata** (v0.12), and **selective
  instrumentation** (v0.13) done — compute-bound overhead fell ~14x → ~1.1x and
  the allocator path ~800x → ~7.5x (`docs/benchmarks.md`), with a `bench.sh
  --check` regression gate now in CI. **v0.14** added `aligned_alloc`/
  `posix_memalign` and C++ `new`/`delete` coverage; **v0.15** added external
  (non-static) global coverage; **v0.16** added stack traces + leak suppressions;
  **v0.17** made the runtime **thread-safe** (allocation-table mutex; the
  per-access fast path stays lock-free). Next: cross-block / loop-range check
  elimination (the remaining `gather` overhead) and incremental instrumentation.
- **Also deferred:** external-global underflow, Bazel.
- **Later:** real-world scale (selective/incremental instrumentation), platform.
