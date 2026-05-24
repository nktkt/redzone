# Real-world validation

redzone's correctness is exercised two ways beyond its own `examples/` corpus:

1. **Differential testing against AddressSanitizer** — a curated subset of the
   corpus is built with `-fsanitize=address` and ASan must reach the same verdict
   ([`scripts/test_diff_asan.sh`](../scripts/test_diff_asan.sh), in CI).
2. **Dog-fooding on real open-source C libraries** — documented here.

The goal of dog-fooding is the Horizon 2 exit criterion: *runs cleanly and
usefully on real code*. "Cleanly" = no false positives on correct programs (the
thing that destroys trust); "usefully" = it actually instruments the code and
catches real out-of-bounds accesses. Three libraries with deliberately different
allocation and access profiles were each built under redzone, exercised on a
realistic workload, and probed with an injected overflow to confirm their heap is
genuinely guarded.

| library | ~LOC | profile stressed | result |
|---|---|---|---|
| [cJSON](https://github.com/DaveGamble/cJSON) | 3.2k | pluggable allocator hooks, `sprintf` number formatting, `strdup`/`memcpy` | clean on a parse/mutate/serialize/free workload; injected overflow caught — **after** a fix (below) |
| [inih](https://github.com/benhoyt/inih) | 330 | fixed stack buffers, string parsing, a custom copy loop | clean; 53 stack variables red-zoned, no false positives |
| [stb_ds](https://github.com/nothings/stb) | 1.9k | `realloc`-driven growth, user pointer offset past an in-allocation header, hashmaps | clean; injected overflow on a stretchy buffer caught |

In each case the pass's own diagnostic confirms instrumentation actually happened
(hundreds of access checks, stack variables red-zoned, allocator calls
redirected), so a clean run means "no bugs found," not "nothing was checked."

## The finding: indirect allocators

cJSON did **not** work at first — its entire heap was untracked. It allocates
through pluggable hooks initialized to the *address* of the allocators:

```c
static internal_hooks global_hooks = { malloc, free, realloc };
```

and calls them indirectly (`global_hooks.allocate(size)`). redzone originally
redirected only *direct* `malloc(...)` call sites, so the address-taken
allocators — and thus every cJSON allocation — escaped instrumentation. None of
the toy examples exposed this because they all call `malloc` directly.

The fix (now in the pass) substitutes a malloc-compatible wrapper for an
address-taken `malloc`/`calloc`/`realloc`/`free`, which also rewrites the
allocator's address inside global function-pointer tables like cJSON's hooks.
After it, redzone tracks cJSON's heap: the workload runs clean and an injected
overflow on a cJSON string is reported. The regression is locked in by
[`examples/indirect_malloc.c`](../examples/indirect_malloc.c).

## Known boundary

redzone redirects allocators at the **call/address site in instrumented code**.
An allocation made entirely inside an *uninstrumented* library (one not compiled
with the pass) is therefore not tracked — unlike AddressSanitizer, which
interposes `malloc` at the symbol level. Compiling the relevant library with
redzone (as done here) brings it into scope.

## Reproducing

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
git clone --depth 1 https://github.com/DaveGamble/cJSON
# write a small driver that parses/mutates/frees, then:
scripts/redzone run cJSON/cJSON.c driver.c -o /tmp/cjson_test && /tmp/cjson_test
```

The same pattern works for any small C library: pass its sources plus a driver
to `scripts/redzone run`.
