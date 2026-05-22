# redzone

> A minimal, educational memory-safety detector for C — built as an LLVM instrumentation pass plus a small runtime. Think of it as a tiny [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html).

`redzone` catches **heap memory errors at runtime** by automatically inserting checks into your program at compile time. You don't change a single line of your code — the LLVM pass rewrites it for you, and the runtime decides whether each memory access is legal.

The name comes from the *red zones*: poisoned guard regions placed around every heap allocation. Step outside an allocation and you land in a red zone, and `redzone` reports exactly where it happened.

## What it detects

| Detected ✅ | Out of scope (for now) ❌ |
|---|---|
| Heap buffer overflow (read/write past a `malloc`'d region) | Stack / global buffer overflows |
| Use-after-free (using memory after it was freed) | Memory leaks |
| | Data races (multithreading) |
| | Performance optimization |

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

1. **Instrumentation pass** (LLVM, C++): walks every function and, before each `load`/`store`, injects a call to `__redzone_check(addr, size, is_write)`. It also redirects user `malloc`/`free` calls to the runtime's versions.
2. **Runtime library** (C):
   - `__redzone_malloc` allocates the requested bytes plus surrounding **red zones**, and records `{base, size, freed?}` in a metadata table.
   - `__redzone_free` quarantines the block instead of releasing it immediately, so later access is detectable as **use-after-free**.
   - `__redzone_check` verifies the access falls inside a live region; on a violation it prints a report and aborts.

## Usage

The fastest way to see it work is the bundled demo (builds the pass and runs it
on a valid program, a heap overflow, and a use-after-free):

```bash
./scripts/demo.sh
```

To run redzone on your own program, instrument the IR with `opt`, then link with
the runtime (which must be compiled **without** the pass):

```bash
clang -g -O0 -S -emit-llvm program.c -o program.ll
opt -load-pass-plugin=build/libRedzonePass.so -passes=redzone -S program.ll -o program.instr.ll
clang -g program.instr.ll runtime/redzone_rt.c -o program
./program
```

Example report on a heap overflow:

```
==redzone ERROR: heap-buffer-overflow
  WRITE of size 4 at 0x104851d30
  region: 16-byte allocation [0x104851d20, 0x104851d30)
  0 byte(s) after the region
```

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
to run it, or just `./scripts/demo.sh`.

## Status

🚧 Early development. **Phases 0–1 complete** (`v0.2`): the pass instruments
every load/store and redirects `malloc`/`free`, and the runtime detects
**heap-buffer-overflow** and **use-after-free** on the bundled examples. Next up
(`v0.3`): `file:line` in reports via debug info, then shadow memory for scale
(Horizon 2).

## License

To be decided.
