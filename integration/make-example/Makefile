# Minimal example of integrating redzone into a plain Makefile build.
#
# Build & run from the redzone repo root:
#   make -C integration/make-example
#   ./integration/make-example/demo        # detects a heap-buffer-overflow, exits nonzero
#
# Use as a standalone template by pointing REDZONE_ROOT at a redzone checkout:
#   make REDZONE_ROOT=/path/to/redzone CC=$(brew --prefix llvm)/bin/clang
#
# IMPORTANT: CC must be the Homebrew/upstream Clang whose LLVM matches the pass
# plugin. Apple Clang cannot load the plugin. If plain `clang` on your PATH is
# not the Homebrew one (`clang --version` should say "Homebrew clang"), pass it
# explicitly, e.g. CC=$(brew --prefix llvm)/bin/clang.

# Compiler. Defaults to `clang`; override on the command line if `clang` is not
# the Homebrew Clang (e.g. CC=$(brew --prefix llvm)/bin/clang).
#
# Note: GNU Make pre-defines CC as `cc` (Apple Clang on macOS, which cannot load
# the plugin), so a plain `CC ?= clang` would have no effect. We only keep an
# explicit value the user passed on the command line or set in the environment;
# otherwise we default to clang.
ifeq ($(origin CC),default)
CC := clang
endif
CC ?= clang

# Where the redzone checkout lives. Default assumes this example sits at
# <root>/integration/make-example, so the root is two levels up. Override for
# standalone use.
REDZONE_ROOT ?= ../..

# The pass plugin (built into the redzone build dir) and the runtime source.
PLUGIN  := $(REDZONE_ROOT)/build/libRedzonePass.so
RUNTIME := $(REDZONE_ROOT)/runtime/redzone_rt.c

# -g gives redzone source locations for its reports; -fpass-plugin runs the
# redzone pass during the normal compile of user code.
CFLAGS  ?= -g
RZFLAGS := -fpass-plugin=$(PLUGIN)

.PHONY: all clean

all: demo

# Link the instrumented user object with the (uninstrumented) runtime object.
demo: main.o redzone_rt.o
	$(CC) $(CFLAGS) main.o redzone_rt.o -o demo

# User code: compiled WITH the redzone pass plugin so loads/stores/allocations
# get instrumented.
main.o: main.c
	$(CC) $(CFLAGS) $(RZFLAGS) -c main.c -o main.o

# Runtime: compiled WITHOUT the plugin. Instrumenting it would rewrite its own
# malloc/free and recurse, so it must never be passed -fpass-plugin.
redzone_rt.o: $(RUNTIME)
	$(CC) $(CFLAGS) -c $(RUNTIME) -o redzone_rt.o

clean:
	rm -rf demo *.o *.dSYM
