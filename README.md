# redzone

[![CI](https://github.com/nktkt/redzone/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/redzone/actions/workflows/ci.yml)

> A minimal, educational memory-safety detector for C — built as an LLVM instrumentation pass plus a small runtime. Think of it as a tiny [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html).

`redzone` catches **heap memory errors at runtime** by automatically inserting checks into your program at compile time. You don't change a single line of your code — the LLVM pass rewrites it for you, and the runtime decides whether each memory access is legal.

The name comes from the *red zones*: poisoned guard regions placed around every heap allocation. Step outside an allocation and you land in a red zone, and `redzone` reports exactly where it happened.

## What it detects

| Detected ✅ | Out of scope (for now) ❌ |
|---|---|
| Heap buffer overflow (read/write past a `malloc`'d region) | *Detecting* data races |
| Stack buffer overflow (past a fixed-size local) | Underflow of an *external* global |
| Global buffer overflow (static/internal **and** external globals) | |
| Use-after-free (read/write after a region is freed) | |
| Double-free and invalid-free | |
| Memory leaks (allocations never freed, reported at exit) | |

The runtime is **thread-safe** — it works correctly in multithreaded programs (no
false positives or crashes from concurrent allocations); it just doesn't *detect*
data races, which is a separate analysis.

Keeping the scope tight is deliberate: nail heap bugs first, expand later.

## How it works

```
                 [ your C source ]
                       |
            clang + redzone pass
                       |
   +-------------------+-------------------+
   | 1. Instrumentation pass (C++)         |
   |    Inserts a check before every       |
   |    load / store instruction           |
   +-------------------+-------------------+
                       | link
   +-------------------+-------------------+
   | 2. Runtime library (C)                |
   |    Wraps malloc/free, tracks          |
   |    allocations, validates accesses    |
   +-------------------+-------------------+
                       |
              [ run ] -> on a bad access:
                         report + abort
```

1. **Instrumentation pass** (LLVM, C++): walks every function and, before each `load`/`store`, injects a call to `__redzone_check(addr, size, is_write, file, line)` (skipping accesses it can prove are safe). It also redirects user allocator calls to the runtime's versions (forwarding the allocation-site `file:line`) — `malloc`/`calloc`/`realloc`/`free`, `aligned_alloc`/`posix_memalign`, and C++ `new`/`new[]`/`delete`/`delete[]` — wraps each static stack allocation with red zones (poisoned at function entry, restored before each return), and wraps eligible globals with red zones (poisoned by a startup constructor — internal globals on both sides, external globals with a trailing red zone that keeps the symbol's address stable for other translation units) — so stack and global overflows are caught too.
2. **Runtime library** (C):
   - `__redzone_malloc` allocates the requested bytes plus surrounding **red zones**, marks the user region addressable and the red zones poisoned in **shadow memory**, and records `{base, size, freed?, alloc site}` in a metadata table.
   - `__redzone_free` quarantines the block and poisons its shadow as freed, so later access is detectable as **use-after-free**.
   - `__redzone_check` reads the shadow for the access in **O(1)** (no scanning); on a poisoned byte it consults the table to produce a rich report and aborts.

## Usage

The easiest way is the **`redzone` CLI**, which automates the whole pipeline and
builds the pass plugin on first use:

```bash
./scripts/redzone run examples/heap_overflow.c   # build + run
./scripts/redzone build program.c -o program     # just produce an instrumented binary
./scripts/redzone run program.c -- arg1 arg2     # forward args; exits with the program's code
```

See `./scripts/redzone --help` for all options. To see every detector at once,
the bundled demo runs a valid program, a heap overflow, and a use-after-free:

```bash
./scripts/demo.sh
```

<details>
<summary>Doing it by hand (what the CLI runs under the hood)</summary>

Instrument the IR with `opt`, then link with the runtime (compiled **without**
the pass):

```bash
clang -g -O0 -S -emit-llvm program.c -o program.ll
opt -load-pass-plugin=build/libRedzonePass.so -passes=redzone -S program.ll -o program.instr.ll
clang -g program.instr.ll runtime/redzone_rt.c -o program
./program
```
</details>

Example report on a heap overflow:

```
==redzone ERROR: heap-buffer-overflow
  WRITE of size 4 at 0x1015a1d30
    at examples/heap_overflow.c:14
  0 byte(s) after a 16-byte region [0x1015a1d20, 0x1015a1d30)
    allocated at examples/heap_overflow.c:7
```

## Output formats

By default redzone prints human-readable reports. Set `REDZONE_FORMAT` for
machine-readable output that editors and CI can consume:

```bash
REDZONE_FORMAT=json  ./scripts/redzone run prog.c   # one JSON object per finding (JSON Lines)
REDZONE_FORMAT=sarif ./scripts/redzone run prog.c   # a SARIF 2.1.0 document
```

Findings are written to **stderr** (kept separate from the program's own
stdout). The SARIF output can be uploaded to GitHub code scanning to annotate
pull requests — see **[docs/ci-integration.md](docs/ci-integration.md)** for a
ready-to-use GitHub Actions workflow.

## Stack traces

Every error report includes a **symbolized call stack** at the point of the bad
access, so you see how the program got there — not just the faulting line:

```
==redzone ERROR: heap-buffer-overflow
  WRITE of size 4 at 0x101395b80
    at examples/heap_overflow.c:14
    #stack (most recent call first):
    #0 prog  0x... write_past_end + 40
    #1 prog  0x... main + 120
  0 byte(s) after a 16-byte region [0x101395b70, 0x101395b80)
    allocated at examples/heap_overflow.c:7
```

Traces are captured only when an error is detected, so they add no runtime cost
to clean code. In JSON output each finding gains a `"stack"` array. By default
frames show function names + offsets (C++ names left mangled); set
`REDZONE_SYMBOLIZE=1` for richer `func (in module) (file:line)` frames, resolved
best-effort by `atos` (macOS) / `llvm-symbolizer` (elsewhere) — any frame that
can't be resolved falls back to the function+offset form.

Reports are **colorized** when stderr is a terminal. Color is disabled when the
output is piped or redirected (so logs and CI stay clean), honors the
[`NO_COLOR`](https://no-color.org/) convention, and can be forced either way with
`REDZONE_COLOR=always|never`.

## Suppressions

Known, intentional **leaks** (e.g. a global cache never freed) can be silenced
with a suppression file, so the rest of your program's leaks still surface:

```bash
cat > redzone.supp <<'EOF'
# silence leaks allocated in third-party code
leak:vendor/
leak:cache.c
EOF
REDZONE_SUPPRESSIONS=redzone.supp ./scripts/redzone run prog.c
```

Each `leak:<substring>` rule matches a leak whose allocation file contains the
substring; a leak matching any rule is not reported, and a run whose every leak
is suppressed exits cleanly. Only leaks are suppressible by design — a buffer
overflow or use-after-free is a real bug, so redzone always reports it.

In the text leak report, leaks from the **same allocation site are collapsed**
into one line with a count (so a loop that leaks 10 000 blocks prints one line,
not 10 000); JSON/SARIF still list every leaked block for tooling.

## Excluding code

Sometimes you want to exclude code from access checking — a hot path, or code
that does intentional out-of-bounds pointer math. Two ways:

**Per function, in the source** — mark it `REDZONE_NO_INSTRUMENT` (from
`redzone_rt.h`):

```c
#include "redzone_rt.h"

REDZONE_NO_INSTRUMENT
void hot_path(int *a, int n) { /* no per-access checks emitted here */ }
```

It maps to clang's standard `__attribute__((disable_sanitizer_instrumentation))`,
so code already annotated for AddressSanitizer works unchanged.

**By pattern, without touching the source** — point `REDZONE_IGNORELIST` at a file
of `fun:` / `src:` glob rules (handy for third-party code you can't edit):

```bash
cat > redzone.ignore <<'EOF'
fun:legacy_*        # exclude functions whose name matches
src:*/vendor/*      # exclude whole source files
EOF
REDZONE_IGNORELIST=redzone.ignore ./scripts/redzone run prog.c
```

Either way, the excluded code's **heap allocations are still tracked** (its
`malloc`/`free` are still redirected), so opting out can never corrupt the heap —
it only removes the per-access checks and stack red-zoning. (`src:` matches a
translation unit's source file; `fun:` matches the — possibly mangled — function
name.)

## Roadmap

The near-term goal is a correct heap checker; the long-term goal is a scalable,
production-grade memory-safety platform. See **[ROADMAP.md](ROADMAP.md)** for the
full long-range plan (Horizons 1–5, scaling strategy, and success metrics).

| Phase | Goal |
|---|---|
| 0 | Pass skeleton — print every load/store (prove instrumentation works) |
| 1 | Runtime + malloc wrapper + red zones → **heap-overflow detection** |
| 2 | Quarantine on free → **use-after-free detection** |
| 3 | Use debug info to report `file:line` |
| 4+ | Shadow memory, stack/global coverage, leak detection, CI/scale (see roadmap) |

## Requirements

- LLVM / Clang **with development headers** (Homebrew `llvm` works well)
- CMake (to build the pass plugin)
- A C++17 toolchain and a C compiler
- POSIX threads — the runtime uses a mutex (on macOS it's in libSystem; on Linux
  link the instrumented program with `-pthread`)

> Developed and tested against LLVM 22. Note that in LLVM 22 the plugin header
> lives at `llvm/Plugins/PassPlugin.h` (it was `llvm/Passes/` in older releases).

## Building from source

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
cmake --build build
```

This produces the pass plugin at `build/libRedzonePass.so`. See **Usage** above
to run it, or just `./scripts/demo.sh`. To wire redzone into your own CMake or
Make project, see **[docs/build-integration.md](docs/build-integration.md)**.

## Testing

`./scripts/test.sh` builds and runs a corpus of small programs under
`examples/`, asserting each expected outcome — clean programs must exit 0, and
buggy ones must abort with the matching `==redzone ERROR: <kind>`. It covers
heap-buffer-overflow (read, write, off-by-one, underflow), use-after-free (read
and write), double-free, and invalid-free, plus several valid programs.

## Status

🚧 Active development. redzone detects **heap-**, **stack-** and
**global-buffer-overflow**, **use-after-free**, **double-free**, **invalid-free**
and **memory leaks** across the full C/C++ allocator surface —
`malloc`/`calloc`/`realloc`/`free`, `aligned_alloc`/`posix_memalign`, and C++
`new`/`new[]`/`delete`/`delete[]` (including the C++17 aligned forms) — reporting
the faulting `file:line` and a
**symbolized stack trace** (plus the allocation site for heap bugs). The
per-access check uses **shadow memory** (O(1)). Globals are covered whether
static/internal or external (cross-TU), and the runtime is **thread-safe** (safe
to use in multithreaded programs). Reports are **colorized** (TTY-aware) with
**deduplicated** leak summaries. Instrumented output is **reproducible**, so
incremental builds and compiler caches (**ccache** and **sccache**) work — see
[docs/caching.md](docs/caching.md). It ships a `redzone` CLI, text/JSON/SARIF
output, **leak suppressions**, instrumentation **opt-outs** (a
`REDZONE_NO_INSTRUMENT` attribute and a `REDZONE_IGNORELIST` file), CMake & Make
integration, and a 28-case suite plus format, cross-TU, report, opt-out,
ignore-list, determinism, ccache, sccache, integration, performance-regression,
and race-engine checks in CI. Remaining gaps: *detecting* data races and
underflow of an external global.

Performance: the per-access check is **inlined** over a **direct-mapped shadow**,
the allocator path is **O(1)** per `malloc`/`free` (each block finds its metadata
via a header in its own red zone, no scanning), and the pass uses **selective
instrumentation** — it skips checks it can prove are safe (accesses in-bounds of a
local, redundant rechecks), which both removes the check and lets the optimizer
keep those locals in registers. Compute-bound code now runs at ~1.1× and
allocation-heavy code at ~7.5× (down from ~800×); see
**[docs/benchmarks.md](docs/benchmarks.md)**.

## Contributing

Contributions are welcome — see **[CONTRIBUTING.md](CONTRIBUTING.md)** for how to
build, run the checks, and add a detector. Release history is in
**[CHANGELOG.md](CHANGELOG.md)**.

## License

[MIT](LICENSE) © nktkt
