//===- redzone_race_main.c - program-integration driver for race mode -----===//
//
// The pieces of the race detector that belong to an INSTRUMENTED PROGRAM rather
// than to the reusable engine/runtime library:
//   - a constructor that registers the main thread as the detector's root and
//     installs the exit-time report;
//   - an atexit hook that summarizes any races found and (by default) exits with
//     a distinct nonzero status, the way a sanitizer signals a finding.
//
// This is deliberately a SEPARATE translation unit so the library
// (redzone_race_rt.c) carries no process-global exit behavior -- the
// deterministic unit test links the library WITHOUT this file and so is
// unaffected. Instrumented programs link all three:
//   redzone_race.c  redzone_race_rt.c  redzone_race_main.c
//
//===----------------------------------------------------------------------===//

#include "redzone_race_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void rz_rt_report_atexit(void) {
  unsigned long n = rz_rt_race_count();
  if (!n)
    return;
  fflush(NULL); // flush the program's own output before we report / bail out
  fprintf(stderr, "[redzone] %lu data race(s) detected\n", n);
  // Exit nonzero like a sanitizer reporting a finding, unless asked to keep the
  // program's own status (useful when triaging with REDZONE_RACE_VERBOSE).
  if (!getenv("REDZONE_RACE_NO_EXIT"))
    _exit(66);
}

__attribute__((constructor(101))) static void rz_rt_setup(void) {
  rz_rt_init(); // make the main thread the root (tid 0) before it runs
  atexit(rz_rt_report_atexit);
}
