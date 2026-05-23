# Contributing to redzone

Thanks for your interest! redzone is a small, educational memory-safety detector
for C/C++ — an LLVM instrumentation pass plus a C runtime. This guide covers how
to build it, run the checks, and add to it.

## Prerequisites

- **LLVM / Clang 22** with development headers (Homebrew `llvm` works well). The
  plugin must be built and loaded with the *same* Clang you instrument with —
  Apple Clang cannot load the plugin.
- **CMake** and a C++17 toolchain.
- **POSIX threads** (libSystem on macOS; `-pthread` when linking on Linux).

> Developed and tested on macOS/ARM64 against LLVM 22. In LLVM 22 the plugin
> header is `llvm/Plugins/PassPlugin.h` (it was `llvm/Passes/` before); the source
> handles both via `__has_include`.

## Build

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix llvm)"
cmake --build build          # produces build/libRedzonePass.so
```

The easiest way to try it is the CLI, which builds the plugin on first use:

```bash
./scripts/redzone run examples/heap_overflow.c
./scripts/demo.sh
```

## Run the checks

These are exactly what CI runs; all must pass before a change lands.

```bash
./scripts/test.sh           # the example corpus: each program's expected outcome
./scripts/test_formats.sh   # JSON / SARIF output is well-formed
./scripts/test_cross_tu.sh  # external globals stay ABI-safe across TUs
./scripts/test_reports.sh   # stack traces, suppressions, color, leak dedup
./scripts/bench.sh --check  # performance regression gate (check count + slowdown)
```

`./scripts/bench.sh` (no `--check`) prints the full benchmark table; see
[`docs/benchmarks.md`](docs/benchmarks.md) for methodology.

## Project layout

| Path | What |
|---|---|
| `src/RedzonePass.cpp` | the LLVM pass: inserts checks, redirects allocators, wraps stack/globals |
| `runtime/redzone_rt.{c,h}` | the runtime: shadow memory, allocation table, reports |
| `examples/` | small programs, one per detector outcome (the test corpus) |
| `scripts/` | CLI, demo, and the test/benchmark runners |
| `cmake/`, `integration/` | build-system integration and runnable examples |
| `docs/` | benchmarks, build/CI integration, and design notes |

## Guiding principles

These come from [`ROADMAP.md`](ROADMAP.md); please keep them in mind:

1. **Correctness first — no false positives.** A memory-safety tool earns trust by
   never crying wolf and never missing the bugs it claims to catch. When in doubt,
   instrument *more* (a redundant check is safe; a skipped real bug is not).
2. **Keep the per-access fast path cheap.** The inlined shadow check runs on every
   load/store; new work belongs on the allocator or error/slow path, not there.
3. **Match the surrounding style.** The code is plain, commented C and C++17;
   follow the conventions already in the file you're editing.

## Two things that will bite you

- **The runtime must be compiled *without* the pass.** Otherwise its own
  `malloc`/`free` get instrumented and recurse forever. The pass also skips any
  function or global named `__redzone*` for the same reason.
- **Don't `git add -A` after running the CLI in the repo root** — it drops example
  binaries there. `.gitignore` covers them, but stage files explicitly.

## Adding a detector test

Most behavior is pinned by the example corpus:

1. Add `examples/<name>.c` (or `.cpp`) — keep it minimal and self-contained.
2. Add a line to the `CASES` table in `scripts/test.sh`:
   `"<name>.c:OK"` for a clean program, or `"<name>.c:<error-kind>"` for one that
   must abort with `==redzone ERROR: <error-kind>`.
3. Run `./scripts/test.sh` and confirm it passes.

## Submitting changes

- Branch from `main`, keep commits focused, and write a message that explains the
  *why*, not just the *what*.
- Make sure all the checks above pass locally; CI runs the same set on macOS.
- Open a pull request describing the change and how you tested it.

## License

By contributing, you agree that your contributions are licensed under the
project's [MIT License](LICENSE).
