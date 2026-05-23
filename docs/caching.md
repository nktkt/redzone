# Caching & incremental builds

redzone is designed to fit into large, repeatedly-built codebases: instrumented
output is **reproducible**, so it works with compiler caches (`ccache`,
`sccache`) and incremental builds.

## Reproducible output

Instrumenting the same translation unit with the same flags produces a
**byte-identical** object file. The pass runs in a deterministic order and emits
deterministically-named symbols, so two builds of the same source agree exactly.
This is verified in CI by [`scripts/test_determinism.sh`](../scripts/test_determinism.sh).

Reproducibility is what makes caching *safe*: a cache may only reuse a previous
object when the inputs are truly identical.

## Incremental builds

Because redzone runs as a normal per-file compiler pass (`-fpass-plugin`), your
build system already rebuilds only the translation units that changed — no
special handling is needed. Add or change one `.c` file and only that file is
re-instrumented and recompiled.

## ccache

Using redzone through ccache works, with **one important caveat**: ccache keys
its cache on the compiler, the preprocessed source, and the command-line flags —
which includes the plugin's *path* (`-fpass-plugin=…/libRedzonePass.so`) but
**not the plugin's contents**. If you rebuild the plugin (a new redzone version)
without changing its path, ccache would serve **stale** objects produced by the
*old* instrumentation.

The fix is to list the plugin in `CCACHE_EXTRAFILES`, so its contents are part of
the cache key and a rebuilt plugin correctly busts the cache:

```bash
export CCACHE_EXTRAFILES="$PWD/build/libRedzonePass.so"
ccache clang -O2 -g -fpass-plugin="$PWD/build/libRedzonePass.so" -c foo.c -o foo.o
```

With that set, an unchanged source is a cache hit (fast), a changed source or a
rebuilt plugin is a miss (correct). This is exercised in CI by
[`scripts/test_ccache.sh`](../scripts/test_ccache.sh).

### With CMake

```cmake
set(CMAKE_C_COMPILER_LAUNCHER ccache)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
```

and export `CCACHE_EXTRAFILES` (pointing at the built plugin) in the environment
that runs the build. See [`cmake/Redzone.cmake`](../cmake/Redzone.cmake) and
[build-integration.md](build-integration.md) for wiring redzone into the compile
itself.

### With Make

```makefile
CC = ccache clang
export CCACHE_EXTRAFILES = $(abspath build/libRedzonePass.so)
```

## sccache and distributed builds

`sccache` caches on the same principles. It does not have a direct
`CCACHE_EXTRAFILES` equivalent, so the simplest robust approach is to make the
plugin path encode its version (e.g. `libRedzonePass-<hash>.so`) so a new plugin
changes the command line and naturally misses the cache. The reproducibility
guarantee above is what lets any such cache be used safely.
