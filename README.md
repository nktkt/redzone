# redzone

[![CI](https://github.com/nktkt/redzone/actions/workflows/ci.yml/badge.svg)](https://github.com/nktkt/redzone/actions/workflows/ci.yml)

> A minimal, educational memory-safety detector for C — built as an LLVM instrumentation pass plus a small runtime. Think of it as a tiny [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html).

`redzone` catches **heap memory errors at runtime** by automatically inserting checks into your program at compile time. You don't change a single line of your code — the LLVM pass rewrites it for you, and the runtime decides whether each memory access is legal.

The name comes from the *red zones*: poisoned guard regions placed around every heap allocation. Step outside an allocation and you land in a red zone, and `redzone` reports exactly where it happened.

## What it detects

| Detected ✅ | Out of scope (for now) ❌ |
|---|---|
| Heap buffer overflow (read/write past a `malloc`'d region) | External (non-static) globals |
| Stack buffer overflow (past a fixed-size local) | Data races (multithreading) |
| Global buffer overflow (past a static/internal global) | Performance optimization |
| Use-after-free (read/write after a region is freed) | |
| Double-free and invalid-free | |
| Memory leaks (allocations never freed, reported at exit) | |

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

1. **Instrumentation pass** (LLVM, C++): walks every function and, before each `load`/`store`, injects a call to `__redzone_check(addr, size, is_write, file, line)`. It also redirects user `malloc`/`calloc`/`realloc`/`free` calls to the runtime's versions (forwarding the allocation-site `file:line`), wraps each static stack allocation with red zones (poisoned at function entry, restored before each return), and wraps eligible static/internal globals with red zones (poisoned by a startup constructor) — so stack and global overflows are caught too.
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

🚧 Active development. **Through `v0.9`:** redzone detects **heap-**, **stack-**
and **global-buffer-overflow**, **use-after-free**, **double-free**,
**invalid-free** and **memory leaks** across `malloc`/`calloc`/`realloc`/`free`,
reporting the faulting `file:line` (plus the allocation site for heap bugs). The
per-access check uses **shadow memory** (O(1)). It ships a `redzone` CLI,
text/JSON/SARIF output, CMake & Make integration, and a 15-case suite plus
format and integration checks in CI. Remaining gaps: external (non-static)
globals, `aligned_alloc` / C++ `new`/`delete`, and threading.

## License

To be decided.
