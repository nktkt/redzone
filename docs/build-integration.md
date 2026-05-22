# Integrating redzone into your build system

redzone is an LLVM pass plugin plus a small runtime. To check a program for
memory-safety bugs you compile your code with the pass enabled, link in the
runtime, and run the resulting binary: a buggy run prints
`==redzone ERROR: <kind>` to stderr and aborts (nonzero exit); a clean run
exits 0.

This guide shows two ways to wire that into a build: the **CMake** module and a
plain **Makefile**. Both produce the same instrumented binary; pick whichever
matches your project.

## Prerequisites

- **Homebrew / upstream Clang.** The compiler that builds your code must be the
  Clang whose LLVM matches the one the pass plugin was built against. Apple
  Clang **cannot** load the plugin. On macOS, install and use Homebrew LLVM:

  ```bash
  brew install llvm
  $(brew --prefix llvm)/bin/clang --version   # should say "Homebrew clang"
  ```

  Either put `$(brew --prefix llvm)/bin` on your `PATH` so plain `clang`
  resolves to it, or pass the full path explicitly to your build (see below).

- **Build the pass plugin first.** Everything depends on
  `<REDZONE_ROOT>/build/libRedzonePass.so`. Build it once:

  ```bash
  cmake -S <REDZONE_ROOT> -B <REDZONE_ROOT>/build \
        -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
  cmake --build <REDZONE_ROOT>/build
  ```

  (`<REDZONE_ROOT>` is your redzone checkout.)

## How it works

- redzone runs as a Clang pass plugin via `-fpass-plugin=<plugin>` during the
  **normal compile** of your source files. No separate `opt` pass is needed —
  the instrumentation happens inline as Clang lowers each `.c` to an object
  file.
- The **runtime** (`runtime/redzone_rt.c`) must be compiled **without** the
  plugin. It provides redzone's own `malloc`/`free` wrappers and shadow-memory
  bookkeeping; instrumenting it would rewrite its own allocations and recurse.
- `-g` is recommended so findings carry source file/line locations.

So the recipe is always: *user code with the plugin* + *runtime without the
plugin* + *link them together*.

## CMake

A drop-in module lives at [`cmake/Redzone.cmake`](../cmake/Redzone.cmake). It
defines `redzone_enable(<target>)`, which adds `-g -fpass-plugin=...` to the
target and links the (uninstrumented) runtime.

In your `CMakeLists.txt`:

```cmake
set(REDZONE_ROOT /path/to/redzone)          # your redzone checkout
include(${REDZONE_ROOT}/cmake/Redzone.cmake)

add_executable(myprog main.c)
redzone_enable(myprog)                       # instrument + link the runtime
```

Configure with the Homebrew Clang:

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang
cmake --build build
./build/myprog
```

The `-DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang` flag is required so the
target is built with the matching Clang. `redzone_enable` errors out early if it
is invoked with a non-Clang compiler or if the plugin has not been built yet.

A complete, runnable example is in
[`integration/cmake-example/`](../integration/cmake-example/) (a `CMakeLists.txt`
plus a buggy `main.c`). Build and run it from the repo root:

```bash
cmake -S integration/cmake-example -B /tmp/rz-cmake \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang
cmake --build /tmp/rz-cmake
/tmp/rz-cmake/demo        # detects a heap-buffer-overflow, exits nonzero
```

## Make

For a plain Makefile, compile user objects with the plugin, the runtime object
without it, and link. The essential pattern:

```make
CC      ?= clang                                  # must be the Homebrew Clang
REDZONE_ROOT ?= /path/to/redzone
PLUGIN  := $(REDZONE_ROOT)/build/libRedzonePass.so
RUNTIME := $(REDZONE_ROOT)/runtime/redzone_rt.c

demo: main.o redzone_rt.o
	$(CC) -g main.o redzone_rt.o -o demo

# user code WITH the plugin
main.o: main.c
	$(CC) -g -fpass-plugin=$(PLUGIN) -c main.c -o main.o

# runtime WITHOUT the plugin
redzone_rt.o: $(RUNTIME)
	$(CC) -g -c $(RUNTIME) -o redzone_rt.o
```

> Note: GNU Make pre-defines `CC` as `cc` (Apple Clang on macOS), so a bare
> `CC ?= clang` has no effect. The example Makefile guards against this with an
> `ifeq ($(origin CC),default)` check; otherwise pass
> `CC=$(brew --prefix llvm)/bin/clang` on the command line.

A complete, runnable example is in
[`integration/make-example/`](../integration/make-example/) (a `Makefile` plus a
buggy `main.c`). Build and run it from the repo root:

```bash
make -C integration/make-example
./integration/make-example/demo        # detects a heap-buffer-overflow, exits nonzero
make -C integration/make-example clean # remove build artifacts
```

If plain `clang` on your `PATH` is not the Homebrew Clang, pass it explicitly:

```bash
make -C integration/make-example CC=$(brew --prefix llvm)/bin/clang
```

## See also

- [`docs/ci-integration.md`](ci-integration.md) — uploading redzone findings to
  GitHub code scanning (SARIF) for *your* real code.
