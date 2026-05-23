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

  //==========================================================================
  // State machine: drive the engine through explicit threads, a per-location
  // shadow, and synchronization events -- still fully deterministic.
  //==========================================================================
  const uintptr_t X = 0x4000;
  const uintptr_t Y = 0x8000;

  // A. Two unsynchronized writes to the same location from different threads.
  {
    rz_race_state st;
    check("sm: state init", rz_race_state_init(&st), 0);
    rz_thread a, b;
    rz_thread_init(&st, &a); // tid 0
    rz_thread_init(&st, &b); // tid 1
    rz_race_access(&st, &a, X, 1);
    int r = rz_race_access(&st, &b, X, 1);
    check("sm: unsync write/write -> race", r, 1);
    rz_race_state_destroy(&st);
  }

  // B. The same two writes, but ordered by a mutex -> no race.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a, b;
    rz_thread_init(&st, &a);
    rz_thread_init(&st, &b);
    rz_sync mtx;
    rz_sync_init(&mtx);
    rz_mutex_acquire(&a, &mtx);
    rz_race_access(&st, &a, X, 1);
    rz_mutex_release(&a, &mtx);
    rz_mutex_acquire(&b, &mtx);
    int r = rz_race_access(&st, &b, X, 1);
    rz_mutex_release(&b, &mtx);
    check("sm: mutex-ordered write/write -> no race", r, 0);
    rz_race_state_destroy(&st);
  }

  // C. Publication: a writes data then releases; b acquires then reads -> the
  //    read sees the write through the release/acquire edge -> no race.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a, b;
    rz_thread_init(&st, &a);
    rz_thread_init(&st, &b);
    rz_sync flag;
    rz_sync_init(&flag);
    rz_race_access(&st, &a, X, 1); // produce
    rz_mutex_release(&a, &flag);   // publish
    rz_mutex_acquire(&b, &flag);   // observe
    int r = rz_race_access(&st, &b, X, 0); // consume (read)
    check("sm: publication read -> no race", r, 0);
    rz_race_state_destroy(&st);
  }

  // D. Thread create/join ordering: the parent's pre-create write is ordered
  //    before the child, and the child's write before the post-join parent read.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread parent;
    rz_thread_init(&st, &parent);
    rz_race_access(&st, &parent, X, 1); // before create
    rz_thread child;
    rz_thread_create(&st, &parent, &child);
    int r1 = rz_race_access(&st, &child, X, 1); // create-ordered
    check("sm: create-ordered write -> no race", r1, 0);
    rz_race_access(&st, &child, Y, 1);
    rz_thread_join(&parent, &child);
    int r2 = rz_race_access(&st, &parent, Y, 0); // join-ordered
    check("sm: join-ordered read -> no race", r2, 0);
    rz_race_state_destroy(&st);
  }

  // E. Concurrent read then write to the same location -> race.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a, b;
    rz_thread_init(&st, &a);
    rz_thread_init(&st, &b);
    rz_race_access(&st, &a, X, 0);          // read
    int r = rz_race_access(&st, &b, X, 1);  // concurrent write
    check("sm: unsync read/write -> race", r, 1);
    rz_race_state_destroy(&st);
  }

  // F. Concurrent read/read -> no race.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a, b;
    rz_thread_init(&st, &a);
    rz_thread_init(&st, &b);
    rz_race_access(&st, &a, X, 0);
    int r = rz_race_access(&st, &b, X, 0);
    check("sm: concurrent read/read -> no race", r, 0);
    rz_race_state_destroy(&st);
  }

  // G. Different locations never interfere, even fully unsynchronized.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a, b;
    rz_thread_init(&st, &a);
    rz_thread_init(&st, &b);
    rz_race_access(&st, &a, X, 1);
    int r = rz_race_access(&st, &b, Y, 1); // different word
    check("sm: distinct locations -> no race", r, 0);
    rz_race_state_destroy(&st);
  }

  // H. Two sequential writes by the SAME thread are program-ordered -> no race.
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a;
    rz_thread_init(&st, &a);
    rz_race_access(&st, &a, X, 1);
    int r = rz_race_access(&st, &a, X, 1);
    check("sm: same-thread write/write -> no race", r, 0);
    rz_race_state_destroy(&st);
  }

  // I. A write released under a mutex still races with a thread that never
  //    acquired it (no happens-before edge to that thread).
  {
    rz_race_state st;
    rz_race_state_init(&st);
    rz_thread a, b, c;
    rz_thread_init(&st, &a);
    rz_thread_init(&st, &b);
    rz_thread_init(&st, &c);
    rz_sync mtx;
    rz_sync_init(&mtx);
    rz_mutex_acquire(&a, &mtx);
    rz_race_access(&st, &a, X, 1);
    rz_mutex_release(&a, &mtx);
    // b synchronizes through the mutex -> ordered.
    rz_mutex_acquire(&b, &mtx);
    int rb = rz_race_access(&st, &b, X, 1);
    rz_mutex_release(&b, &mtx);
    check("sm: mutex acquirer -> no race", rb, 0);
    // c never touches the mutex -> still concurrent with b's write -> race.
    int rc = rz_race_access(&st, &c, X, 1);
    check("sm: non-acquirer -> race", rc, 1);
    rz_race_state_destroy(&st);
  }

  printf("\n");
  if (failures == 0) {
    printf("race-engine: all scenarios passed\n");
    return 0;
  }
  printf("race-engine: %d scenario(s) FAILED\n", failures);
  return 1;
}
