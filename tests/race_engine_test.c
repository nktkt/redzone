// Deterministic unit test for the happens-before engine (runtime/redzone_race).
// No real threads: each scenario constructs vector clocks and a recorded access
// by hand and asserts the race decision, so the correctness-critical core is
// validated with zero concurrency and zero flakiness.
#include "redzone_race.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, int got, int want) {
  if (got == want) {
    printf("PASS  %s\n", name);
  } else {
    printf("FAIL  %s (got %d, want %d)\n", name, got, want);
    failures++;
  }
}

int main(void) {
  // T0 writes location X at its epoch 1.
  rz_vc t0;
  rz_vc_init(&t0);
  rz_vc_tick(&t0, 0);
  rz_access wrote_x = {.valid = 1, .tid = 0, .epoch = t0.t[0], .is_write = 1};
  rz_access read_x = {.valid = 1, .tid = 0, .epoch = t0.t[0], .is_write = 0};

  // 1. Two unsynchronized writes from different threads -> RACE.
  rz_vc t1;
  rz_vc_init(&t1);
  rz_vc_tick(&t1, 1);
  check("write/write concurrent -> race", rz_race_check(&wrote_x, 1, &t1, 1), 1);

  // 2. Ordered by a mutex (release/acquire) -> NO race. T0 unlocks M publishing
  //    its clock; T1 locks M (joins it in) before writing.
  rz_vc m;
  rz_vc_copy(&m, &t0); // unlock M  := publish T0's clock
  rz_vc t1_locked;
  rz_vc_init(&t1_locked);
  rz_vc_tick(&t1_locked, 1);
  rz_vc_join(&t1_locked, &m); // lock M := acquire
  check("write/write mutex-ordered -> no race",
        rz_race_check(&wrote_x, 1, &t1_locked, 1), 0);

  // 3. Read/read is never a race, even concurrent.
  check("read/read concurrent -> no race", rz_race_check(&read_x, 1, &t1, 0), 0);

  // 4. A prior read and a concurrent write conflict -> RACE.
  check("read/write concurrent -> race", rz_race_check(&read_x, 1, &t1, 1), 1);

  // 5. Two accesses by the same thread are program-ordered -> no race.
  check("same thread -> no race", rz_race_check(&wrote_x, 0, &t0, 1), 0);

  // 6. Thread-create ordering: the child inherits the parent's clock at create,
  //    so the parent's pre-create write is ordered before the child -> no race.
  rz_vc child;
  rz_vc_copy(&child, &t0); // child inherits parent's clock at create
  rz_vc_tick(&child, 1);   // child advances its own epoch
  check("create-ordered -> no race", rz_race_check(&wrote_x, 1, &child, 1), 0);

  // 7. A later (higher-epoch) unsynchronized write still races.
  rz_vc t1_more;
  rz_vc_init(&t1_more);
  rz_vc_tick(&t1_more, 1);
  rz_vc_tick(&t1_more, 1);
  check("concurrent regardless of epoch -> race",
        rz_race_check(&wrote_x, 1, &t1_more, 1), 1);

  // 8. No prior access recorded -> no race.
  rz_access none = {0};
  check("no prior access -> no race", rz_race_check(&none, 1, &t1, 1), 0);

  printf("\n");
  if (failures == 0) {
    printf("race-engine: all scenarios passed\n");
    return 0;
  }
  printf("race-engine: %d scenario(s) FAILED\n", failures);
  return 1;
}
