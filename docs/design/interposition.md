# Design note: whole-process malloc interposition

Status: **experimental, working on macOS** (`scripts/test_interpose.sh`, in CI;
the default compile-time mode is unaffected). An overflow on memory allocated by
an *uninstrumented* library — invisible to a normal build — is caught once the
process is run under the interposer; a correct program runs clean. Code:
`runtime/redzone_interpose.c` + the `rz_sys_*` / `__redzone_owns` /
`__redzone_maybe_owns` / `__redzone_usable_size` / `__redzone_in_runtime` /
`__redzone_set_leak_report` runtime support. Not yet wired into the `redzone` CLI
(run it by hand, below).

## How to use

```sh
clang -O2 -dynamiclib runtime/redzone_rt.c \
    -install_name @rpath/libredzone_rt.dylib -o libredzone_rt.dylib
clang -O2 -dynamiclib runtime/redzone_interpose.c -L. -lredzone_rt \
    -Wl,-rpath,. -install_name @rpath/libredzone_interpose.dylib \
    -o libredzone_interpose.dylib
# build your program against libredzone_rt (instrument the parts you want checked
# with the pass, as usual), then:
DYLD_INSERT_LIBRARIES=./libredzone_interpose.dylib ./your_program
```

## What it took (the three problems, and the fixes)

The naive "interpose `malloc`, register a zone" approach crashed on startup. Three
fixes, each a small increment, made it work:

1. **Startup introspection** — macOS code (libobjc) calls `malloc_size()` /
   `malloc_zone_from_ptr()` during very early image load, *before* any constructor
   could register a zone; our offset user pointers then report size 0 → "corrupt
   data pointer" abort. Fixed by **interposing the introspection functions
   themselves** (active from load): a redzone block answers via
   `__redzone_usable_size`, foreign pointers fall back to the real default zone.
2. **Re-entrancy / deadlock** — the runtime holds its table lock and then calls
   libc functions that allocate (e.g. `backtrace_symbols` on the error path);
   under interposition that re-enters the interposed `malloc` and tries to take
   the table lock again. Fixed with a per-thread guard (`__redzone_in_runtime`,
   raised across every locked section): the interposer forwards any re-entrant
   allocation straight to the system allocator (`rz_sys_*`), using the lock-free
   `__redzone_maybe_owns` for the free path.
3. **Exit-time false leaks** — under interposition every live process allocation
   (all of libsystem) is a redzone block at exit, so the leak report would flag
   them all. The interposer disables it (`__redzone_set_leak_report(0)`).

Overhead is modest in practice (the test program runs in well under a second).

## Remaining work / caveats

- **CLI integration** — a `redzone run --interpose` mode that builds against the
  runtime dylib and sets `DYLD_INSERT_LIBRARIES`.
- **Re-entrant `realloc`/`free` of a redzone block** while the lock is held is
  assumed not to occur (runtime-internal allocations are system blocks); a
  redzone block there is leaked rather than mishandled.
- **macOS only** (dyld `__interpose`). A Linux port would use symbol
  interposition (`--wrap` or a preloaded `.so` with `dlsym(RTLD_NEXT)`).
- Cross-process / leak detection under interposition is intentionally off.

The analysis below records *why* the naive approach failed, for posterity.

## Goal

redzone redirects allocators at the **call (or address-of) site in instrumented
code**. Anything allocated entirely inside an *uninstrumented* library — one not
compiled with the pass — is therefore untracked: its memory has no red zones, so
even an instrumented access to it can't be checked, and double-free / leak
tracking is incomplete across the boundary.

Whole-process **interposition** would close that: route *every* allocation in the
process — including those made deep inside prebuilt libraries — through the
redzone runtime, the way AddressSanitizer does. Then library-allocated memory
gets red zones and instrumented accesses to it are checked, and free/UAF/leak
tracking becomes process-wide.

## Approach (and what the spike established works)

On macOS the mechanism is dyld's `__interpose` section, in a dylib loaded via
`DYLD_INSERT_LIBRARIES`, replacing `malloc`/`calloc`/`realloc`/`free`
process-wide. The runtime is built as a **shared dylib** so the interposer and
the instrumented program share one shadow.

The spike got the core mechanics working:

- **Routing.** A controlled test — an *uninstrumented* library allocator plus an
  *instrumented* access that overflows the block — confirms the gap and the fix:
  without interposition the overflow is missed (library `malloc`, no red zone);
  the interpose routes that `malloc` through the runtime.
- **Recursion.** The runtime's own internal allocations would re-enter the
  interposed `malloc` and recurse forever. Solved by `rz_sys_*`: the runtime
  allocates through the **default malloc zone** (`malloc_zone_malloc(...)`), a
  symbol dyld's interpose does not replace. (This is transparent in normal builds
  — the full test suite is unaffected.)
- **Foreign pointers.** `free`/`realloc` of a pointer redzone didn't allocate
  (pre-init, or another zone) are forwarded to the real allocator via
  `__redzone_owns`, rather than erroring.

## The blocker: macOS allocation introspection + bootstrapping

Interposing `malloc` alone crashes macOS programs (even pure C ones, which still
link libobjc/libsystem):

```
objc[...]: realized class 0x... has corrupt data pointer: malloc_size(0x...) = 0
```

macOS code introspects allocations with `malloc_size()` / `malloc_zone_from_ptr()`,
which walk the **registered malloc zones** asking each "is this pointer yours, and
how big?". redzone hands back a user pointer offset past an in-allocation header,
which the *default* zone doesn't recognize → size 0 → "corrupt data pointer".

Registering a redzone `malloc_zone_t` with a `size` callback that recognizes our
blocks (implemented in the spike) is necessary but **not sufficient**: objc
realizes classes during very early image load, *before* any `__attribute__((
constructor))` — even `constructor(0)` — can register the zone. So the earliest
interposed allocations are redzone blocks that no registered zone yet claims, and
introspection of them still fails.

## Path forward

A robust implementation is essentially AddressSanitizer's macOS allocator:

1. Install redzone **as the default malloc zone** (not merely an additional
   registered zone), extremely early — before libobjc/libsystem initialize —
   typically from the inserted dylib's load, ahead of constructors.
2. Provide the **full** `malloc_zone_t` + `malloc_introspection_t` (size,
   enumerator, statistics, batch ops, lock/unlock around `fork`, …).
3. A **bootstrap allocator** (a small static pool) to satisfy allocations that
   occur before the runtime is initialized, as ASan does.

This is a substantial, delicate sub-project with real risk of subtle breakage,
which is why it is scoped out here rather than half-built. `redzone_interpose.c`
is the documented starting point: the interpose tuples, the recursion-safe
`rz_sys_*` allocation, foreign-pointer forwarding, and the zone with a `size`
callback are all in place; what remains is early default-zone installation and
bootstrapping.

## Relationship to the rest of redzone

This is independent of, and complementary to, the compile-time call-site / address
redirection (which already covers allocators used by code you build with redzone,
including indirect/function-pointer allocators — see
[`real-world-validation.md`](../real-world-validation.md)). Interposition would
extend coverage to fully prebuilt libraries. The default, compile-time mode is
unaffected by any of this groundwork.
