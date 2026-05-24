# redzone tutorial — find your first memory bug in 5 minutes

This is a hands-on walkthrough of **redzone**, an LLVM-based memory-safety
detector for C/C++ (a "mini AddressSanitizer") with an opt-in data-race detector.
Every command below is run from the repository root and uses the bundled
`examples/`. For the reference documentation, see the [README](../README.md).

## 1. Prerequisites

- **macOS** with **Homebrew LLVM** (the pass is a clang plugin; Apple's clang
  can't load it). Install with `brew install llvm cmake`.
- Put the LLVM tools on your `PATH` so `clang` and `opt` resolve to Homebrew's
  (it's keg-only):

  ```sh
  export PATH="$(brew --prefix llvm)/bin:$PATH"
  ```

That's it — the `redzone` CLI builds the pass plugin automatically the first time
you use it.

## 2. Catch your first bug

`scripts/redzone run` instruments a source file, links the runtime, and runs it:

```sh
$ scripts/redzone run examples/heap_overflow.c
==redzone ERROR: heap-buffer-overflow
  WRITE of size 4 at 0x104a11b50
    at examples/heap_overflow.c:14
    #stack (most recent call first):
    #0 ho   main + 508
  0 byte(s) after a 16-byte region [0x..., 0x...)
    allocated at examples/heap_overflow.c:7
```

You get the **kind** of bug, the **faulting line**, a **stack trace**, and — for
heap bugs — the **allocation site**. The process exits nonzero. A clean program
just runs:

```sh
$ scripts/redzone run examples/valid.c
sum = 14
$ echo $?
0
```

`scripts/redzone build <src>` does the same without running, producing an
executable you can run yourself.

## 3. What it detects

Try any of these — each aborts with a precise report:

| example | bug |
|---|---|
| `heap_overflow.c`, `off_by_one_read.c`, `underflow_write.c` | heap buffer overflow |
| `use_after_free.c`, `use_after_free_write.c` | use-after-free |
| `double_free.c`, `invalid_free.c` | bad `free` |
| `stack_overflow.c` | stack buffer overflow |
| `global_overflow.c` | global buffer overflow |
| `memory_leak.c` | memory leak (reported at exit) |

```sh
$ scripts/redzone run examples/use_after_free.c
==redzone ERROR: use-after-free
  READ of size 4 at 0x...
    at examples/use_after_free.c:14
```

Coverage spans the full allocator surface — `malloc`/`calloc`/`realloc`/`free`,
`aligned_alloc`/`posix_memalign`, and C++ `new`/`delete` (including the C++17
aligned forms).

## 4. Overflows through library calls

A single `memcpy` or `strcpy` is one opaque call, so a naive checker never sees
the out-of-bounds bytes. redzone intercepts the bulk-memory and string functions
and bounds-checks their ranges:

```sh
$ scripts/redzone run examples/memcpy_overflow.c
==redzone ERROR: heap-buffer-overflow
  WRITE of size 16 at 0x...
    at examples/memcpy_overflow.c:12
```

This covers `memcpy`/`memmove`/`memset`, `strcpy`/`strcat`/`strncpy`/`strncat`/
`strlcpy`/`strlcat`, and `sprintf`/`snprintf` (and the fortified `__*_chk`
forms the compiler may emit).

## 5. Better reports

- **Symbolized `file:line` in stack frames** (best-effort): set
  `REDZONE_SYMBOLIZE=1` to resolve frames to source positions via `atos` /
  `llvm-symbolizer`.
- **Color** is automatic on a TTY (disable with `NO_COLOR=1`, force with
  `REDZONE_COLOR=always`).

## 6. Machine-readable output (for CI)

Set `REDZONE_FORMAT=json` (JSON Lines) or `REDZONE_FORMAT=sarif` (SARIF 2.1.0,
for GitHub code scanning) to emit structured findings on stderr:

```sh
$ REDZONE_FORMAT=json scripts/redzone run examples/heap_overflow.c
{"tool":"redzone","error":"heap-buffer-overflow","access":"write","size":4,
 "location":{"file":"examples/heap_overflow.c","line":14},
 "region":{"size":16,"allocated":{"file":"examples/heap_overflow.c","line":7}},
 "stack":[...]}
```

See [`docs/ci-integration.md`](ci-integration.md) for a SARIF → GitHub
code-scanning recipe.

## 7. Find data races

redzone also has an opt-in **data-race detector** (happens-before, the model
ThreadSanitizer uses). Add `--race`:

```sh
$ scripts/redzone run --race examples/race_data_race.c
==redzone WARNING: data race
  write by thread 2 at examples/race_data_race.c:14
  previous write by thread 1 at examples/race_data_race.c:14
  address 0x...
```

It reports **both** conflicting accesses with `file:line`, and understands the
happens-before edges from `pthread_create`/`join`, mutexes (incl. `trylock`),
reader/writer locks, condition variables, and C/C++ atomics — so correctly
synchronized code (e.g. `examples/race_clean.c`) runs clean. It's a separate,
heavier mode; see [`docs/design/data-race-detection.md`](design/data-race-detection.md).

## 8. Use it in your own project

- **Build systems:** drop-in CMake and Make integration via `-fpass-plugin`. See
  [`docs/build-integration.md`](build-integration.md) and the runnable
  `integration/cmake-example/` and `integration/make-example/`.
- **CI:** [`docs/ci-integration.md`](ci-integration.md) shows wiring SARIF output
  into GitHub code scanning.

## 9. Tuning what gets checked

- **Per-function opt-out:** annotate a hot or pointer-tricky function with
  `__attribute__((disable_sanitizer_instrumentation))` (or the
  `REDZONE_NO_INSTRUMENT` macro from `runtime/redzone_rt.h`). Its allocations are
  still tracked, so the heap stays consistent.
- **File-level ignore-list:** point `REDZONE_IGNORELIST` at a file of
  `fun:<glob>` / `src:<glob>` rules to exclude code you can't annotate.
- **Leak suppressions:** `REDZONE_SUPPRESSIONS` silences known leaks by
  allocation file (hard errors are never suppressed). See the README's
  [Suppressions](../README.md#suppressions) and
  [Excluding code](../README.md#excluding-code) sections.

## Where to go next

- [README](../README.md) — full feature reference.
- [ROADMAP.md](../ROADMAP.md) — the long-range plan (Horizons 1–5).
- [`docs/design/`](design/) — design notes (shadow memory, selective
  instrumentation, the data-race detector).
- Run the test suite: `./scripts/test.sh` (it builds the plugin on first use).
