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
- **Full allocator coverage** — `calloc`, `realloc`, `aligned_alloc`,
  `free`, and C++ `new`/`delete`.
- **Stack buffer overflow** detection (red zones around stack allocations).
- **Global buffer overflow** detection.
- **Memory leak detection** at exit (LeakSanitizer-style).
- **Performance budget** — target ≤ 2–3× slowdown; track memory overhead.

**Exit criteria:** runs cleanly and usefully on a mid-size open-source C project.

## Horizon 3 — Developer experience & tooling · `v0.7`–`v0.9`

Turn a compiler pass into a product people choose to use.

- **CLI / compiler wrapper** — one command to build with redzone enabled.
- **Great reports** — symbolized, colorized, deduplicated, with stack traces.
- **Suppression files** — silence known/third-party issues.
- **Machine-readable output** — JSON and **SARIF** for tooling.
- **Build-system recipes** — CMake, Bazel, Make.
- **CI integrations** — GitHub Actions / GitLab CI; PR annotations via SARIF.
- **Docs site** — tutorials, examples, troubleshooting.

**Exit criteria:** a new user adopts redzone in an existing project in < 15 min.

## Horizon 4 — Scale · `v1.0`+

Run on huge codebases and many builds without pain.

- **Selective instrumentation** — skip provably-safe accesses via static
  analysis; per-file/per-function opt-out attributes.
- **Incremental builds** — only re-instrument changed translation units;
  compatibility with `ccache`/`sccache` and distributed builds.
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
- **Findings dashboard** — aggregate results across builds, track regressions
  and trends over time, triage workflow. *(The org-scale / SaaS angle.)*
- **Fuzzing integration** — coverage-guided fuzzing with redzone as the crash
  oracle.
- **IDE integration** — inline diagnostics (VS Code extension).
- **Cloud offering** — org-wide policy, historical analytics, regression gating.

---

## Cross-cutting tracks (continuous)

Run alongside every horizon, not in sequence.

- **Testing & correctness** — golden corpus of bad/good programs; track
  false-positive / false-negative rates; differential testing against real
  AddressSanitizer.
- **Benchmarking** — continuous perf/memory-overhead benchmarks in CI.
- **Release engineering** — CI/CD, prebuilt binaries, packaging (Homebrew, apt).
- **Security & hardening** — the tool itself must be robust against the inputs
  it analyzes.
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

- **Done:** `v0.1`–`v0.3` (overflow + use-after-free detection, readable
  reports, 10-case test suite); `v0.4` shadow memory (O(1) per-access check).
- **Now:** broaden detection — stack/global buffer overflows, then memory leaks.
- **Next:** developer experience — a CLI wrapper, SARIF/JSON output, CI recipes.
- **Later:** real-world scale (selective/incremental instrumentation), platform.
