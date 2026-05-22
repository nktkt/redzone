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

1. **Instrumentation pass** (LLVM, C++): walks every function and, before each `load`/`store`, injects a call to `__redzone_check(addr, size, is_write, location_id)`.
2. **Runtime library** (C):
   - `__redzone_malloc` allocates the requested bytes plus surrounding **red zones**, and records `{base, size, freed?, alloc_site}` in a metadata table.
   - `__redzone_free` quarantines the block instead of releasing it immediately, so later access is detectable as **use-after-free**.
   - `__redzone_check` verifies the access falls inside a live region; on a violation it prints a report and aborts.

## Usage

```bash
clang -fpass-plugin=./redzone.so -g program.c redzone_runtime.c -o program
./program
```

Example report on a heap overflow:

```
==redzone ERROR: heap-buffer-overflow
  WRITE of size 4 at address 0x55a3...
  at program.c:12
  0x55a3... is 4 bytes after a 40-byte region allocated at program.c:8
```

## Roadmap

| Phase | Goal |
|---|---|
| 0 | Pass skeleton — print every load/store (prove instrumentation works) |
| 1 | Runtime + malloc wrapper + red zones → **heap-overflow detection** |
| 2 | Quarantine on free → **use-after-free detection** |
| 3 | Use debug info to report `file:line` |
| 4 (stretch) | Shadow memory, stack/global coverage, leak detection |

## Requirements

- LLVM / Clang (for building the pass and compiling instrumented programs)
- CMake (to build the pass plugin)
- A C++17 toolchain and a C compiler

## Status

🚧 Early development. The specification and roadmap are in place; implementation starts at Phase 0.

## License

To be decided.
