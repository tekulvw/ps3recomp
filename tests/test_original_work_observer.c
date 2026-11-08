/* Generic authored identities/PCs only. No title source, addresses or layout.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "original_work_observer.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
static ps3_original_identity identity = {11, 2, 13};
static int rogue;
static uint64_t current_test_worker = 8;
static uint64_t current_test_generation = 1;
static int witness_enabled = 1;
static unsigned witness_repetitions = 1, witness_route = PS3_ORIGINAL_MICROSTEP;
static int archive_test;
static int witness_after_retirement;
static int racing;
static atomic_int race_start;
static int reuse_test, replay_test;
static atomic_int reuse_stage;
static int resolve(const void *context, uint64_t pc, unsigned route,
                   const char *provider, ps3_original_identity *out) {
  (void)pc;
  (void)route;
  assert(provider && *provider);
  *out = *(const ps3_original_identity *)context;
  errno = ERANGE;
  return 0;
}
static void new_worker_body(void) {
  if (replay_test) {
    assert(ps3_original_worker_register(current_test_worker,
                                        current_test_generation) == -1);
    return;
  }
  if (reuse_test) {
    const uint64_t id = current_test_worker;
    assert(!ps3_original_worker_register(id, 1));
    if (id == 7) {
      PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_MICROSTEP);
      assert(!ps3_original_worker_retire());
      atomic_store_explicit(&reuse_stage, 1, memory_order_release);
      while (atomic_load_explicit(&reuse_stage, memory_order_acquire) < 3) {
      }
      PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_MICROSTEP);
      atomic_store_explicit(&reuse_stage, 4, memory_order_release);
    } else {
      atomic_store_explicit(&reuse_stage, 2, memory_order_release);
      while (atomic_load_explicit(&reuse_stage, memory_order_acquire) < 4) {
      }
      PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_MICROSTEP);
      assert(!ps3_original_worker_retire());
    }
    return;
  }
  if (racing) {
    while (!atomic_load_explicit(&race_start, memory_order_acquire)) {
    }
    assert(ps3_original_worker_register(999, 1) == -1);
    for (unsigned i = 0; i < 10000; ++i)
      (void)ps3_original_begin_drain(150);
    return;
  }
  if (!rogue)
    assert(ps3_original_worker_register(current_test_worker,
                                        current_test_generation) == 0);
  if (archive_test) {
    for (unsigned route = 0; route < PS3_ORIGINAL_OUTSIDE; ++route)
      PS3_ORIGINAL_STEP(&identity, 24, route);
  } else if (witness_enabled) {
    for (unsigned n = 0; n < witness_repetitions; ++n)
      PS3_ORIGINAL_STEP(&identity, 24, witness_route);
  }
  if (!rogue)
    assert(ps3_original_worker_retire() == 0);
  if (witness_after_retirement)
    PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_MICROSTEP);
}
#ifdef _WIN32
typedef HANDLE test_thread;
static DWORD WINAPI new_worker(void *arg) {
  (void)arg;
  new_worker_body();
  return 0;
}
static test_thread start_new_worker(void) {
  HANDLE thread = CreateThread(NULL, 0, new_worker, NULL, 0, NULL);
  assert(thread);
  return thread;
}
static void join_new_worker(test_thread thread) {
  assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  assert(CloseHandle(thread));
}
#else
typedef pthread_t test_thread;
static void *new_worker(void *arg) {
  (void)arg;
  new_worker_body();
  return NULL;
}
static test_thread start_new_worker(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, new_worker, NULL) == 0);
  return thread;
}
static void join_new_worker(test_thread thread) {
  assert(pthread_join(thread, NULL) == 0);
}
#endif
static void run_new_worker(void) { join_new_worker(start_new_worker()); }
static void arm_racing_worker(const ps3_original_config *config) {
  racing = 1;
#ifdef _WIN32
  HANDLE thread = CreateThread(NULL, 0, new_worker, NULL, 0, NULL);
  assert(thread);
#else
  pthread_t thread;
  assert(pthread_create(&thread, NULL, new_worker, NULL) == 0);
#endif
  atomic_store_explicit(&race_start, 1, memory_order_release);
  int armed = ps3_original_arm(config);
#ifdef _WIN32
  assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  assert(CloseHandle(thread));
#else
  assert(pthread_join(thread, NULL) == 0);
#endif
  if (!armed) {
    (void)ps3_original_begin_drain(150);
    ps3_original_report report = {.struct_size = sizeof(report),
                                  .abi_version = 1};
    assert(ps3_original_seal(&report) == 0 && report.faults);
  } else
    assert(armed == -1);
  puts("original observer concurrent arm/register/drain is never eligible");
}
static void lifetime_capacity(void) {
  const unsigned actual_workers = 129;
  for (unsigned i = 0; i < actual_workers; ++i) {
    current_test_worker = 100 + i;
    assert(!ps3_original_enroll(current_test_worker, 1));
    run_new_worker();
  }
  ps3_original_live_report live = {.struct_size = sizeof(live),
                                   .abi_version = 1};
  assert(!ps3_original_live_status(&live) && live.workers == actual_workers &&
         !live.faults);
  uint64_t total = 0;
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_MICROSTEP, &total) &&
         total == actual_workers);
  /* Retired identities remain queryable: never recycle an old TLS slot into a
   * different worker. A witness after genuine retirement still fails. */
  witness_after_retirement = 1;
  current_test_worker = 100 + actual_workers;
  assert(!ps3_original_enroll(current_test_worker, 1));
  run_new_worker();
  for (unsigned i = 0; i < PS3_ORIGINAL_MAX_WORKERS; ++i)
    assert(!ps3_original_enroll(100000 + i, 1));
  assert(ps3_original_enroll(100000 + PS3_ORIGINAL_MAX_WORKERS, 1) == -1);
  for (unsigned i = 0; i < PS3_ORIGINAL_MAX_WORKERS; ++i)
    assert(!ps3_original_worker_cancel(100000 + i));
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report));
  assert(report.workers == actual_workers + 1 + PS3_ORIGINAL_MAX_WORKERS);
  assert(report.faults ==
         (PS3_ORIGINAL_MISSING_WORKER | PS3_ORIGINAL_LATE_WORK));
  assert(!ps3_original_count(100, 1, 31, PS3_ORIGINAL_MICROSTEP, &total) &&
         total == 1);
  assert(!ps3_original_count(100 + actual_workers, 1, 31,
                             PS3_ORIGINAL_MICROSTEP, &total) &&
         total == 1);
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_MICROSTEP, &total) &&
         total == actual_workers + 1);
  puts("original observer130 retired workers preserve history;1024 "
       "simultaneous expected slots exhaust safely");
}
static void reused_tls(void) {
  reuse_test = 1;
  current_test_worker = 7;
  assert(!ps3_original_enroll(7, 1));
  test_thread old = start_new_worker();
  while (atomic_load_explicit(&reuse_stage, memory_order_acquire) < 1) {
  }
  current_test_worker = 8;
  assert(!ps3_original_enroll(8, 1));
  test_thread next = start_new_worker();
  while (atomic_load_explicit(&reuse_stage, memory_order_acquire) < 2) {
  }
  atomic_store_explicit(&reuse_stage, 3, memory_order_release);
  join_new_worker(old);
  join_new_worker(next);
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) &&
         report.faults == PS3_ORIGINAL_LATE_WORK);
  uint64_t n = 0;
  assert(!ps3_original_count(7, 1, 31, PS3_ORIGINAL_MICROSTEP, &n) && n == 1);
  assert(!ps3_original_count(8, 1, 31, PS3_ORIGINAL_MICROSTEP, &n) && n == 1);
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_MICROSTEP, &n) && n == 2);
  puts("old retired TLS cannot write into a concurrently active reused counter "
       "slot");
}
static void replay_generation(void) {
  current_test_worker = 7;
  current_test_generation = 3;
  assert(!ps3_original_enroll(7, 3));
  run_new_worker();
  assert(ps3_original_enroll(7, 3) == -1);
  replay_test = 1;
  run_new_worker();
  replay_test = 0;
  current_test_generation = 4;
  assert(!ps3_original_enroll(7, 4));
  run_new_worker();
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) &&
         report.faults == PS3_ORIGINAL_MISSING_WORKER);
  uint64_t n = 0;
  assert(!ps3_original_count(7, 3, 31, PS3_ORIGINAL_MICROSTEP, &n) && n == 1);
  assert(!ps3_original_count(7, 4, 31, PS3_ORIGINAL_MICROSTEP, &n) && n == 1);
  assert(ps3_original_count(7, 5, 31, PS3_ORIGINAL_MICROSTEP, &n) == -1);
  puts("retired identity replay refuses while a distinct generation keeps "
       "independent history");
}
static void history_budget(void) {
  for (unsigned i = 0; i < PS3_ORIGINAL_MAX_LIFETIMES; ++i) {
    assert(!ps3_original_enroll(100 + i, 1));
    assert(!ps3_original_worker_cancel(100 + i));
  }
  assert(ps3_original_enroll(100 + PS3_ORIGINAL_MAX_LIFETIMES, 1) == -1);
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) &&
         report.faults == PS3_ORIGINAL_MISSING_WORKER);
  uint64_t n = 1;
  assert(!ps3_original_count(100, 1, 31, PS3_ORIGINAL_MICROSTEP, &n) && n == 0);
  assert(!ps3_original_count(99 + PS3_ORIGINAL_MAX_LIFETIMES, 1, 31,
                             PS3_ORIGINAL_MICROSTEP, &n) &&
         n == 0);
  assert(ps3_original_count(100 + PS3_ORIGINAL_MAX_LIFETIMES, 1, 31,
                            PS3_ORIGINAL_MICROSTEP, &n) == -1);
  puts("compact lifetime budget exhausts explicitly without overwriting early "
       "or late identities");
}
static void archive_budget(void) {
  archive_test = 1;
  const unsigned lifetimes =
      PS3_ORIGINAL_MAX_ARCHIVED_COUNTS /
          (PS3_ORIGINAL_MAX_TARGETS * PS3_ORIGINAL_ROUTES) +
      2;
  for (unsigned i = 0; i < lifetimes; ++i) {
    current_test_worker = 100 + i;
    assert(!ps3_original_enroll(current_test_worker, 1));
    run_new_worker();
  }
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) && report.faults == PS3_ORIGINAL_OVERFLOW);
  for (unsigned i = 0; i < lifetimes; ++i) {
    for (unsigned t = 0; t < PS3_ORIGINAL_MAX_TARGETS; ++t) {
      uint64_t count = 0;
      assert(!ps3_original_count(100 + i, 1, 31 + t, PS3_ORIGINAL_LIFTED,
                                 &count) &&
             count == 1);
      assert(!ps3_original_count(100 + i, 1, 31 + t, PS3_ORIGINAL_OUTSIDE,
                                 &count) &&
             count == PS3_ORIGINAL_OUTSIDE);
    }
  }
  uint64_t total = 0;
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_LIFTED, &total) &&
         total == lifetimes);
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_OUTSIDE, &total) &&
         total == (uint64_t)lifetimes * PS3_ORIGINAL_OUTSIDE);
  puts("full sparse archive faults and pins remaining dense evidence; every "
       "historical count stays exact");
}
static void aggregate_limit(void) {
  witness_repetitions = 2;
  witness_route = PS3_ORIGINAL_INTERPRETER;
  for (unsigned i = 0; i < 2; ++i) {
    current_test_worker = 7 + i;
    assert(!ps3_original_enroll(current_test_worker, 1));
    run_new_worker();
  }
  uint64_t total = 0;
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_INTERPRETER, &total) &&
         total == 4);
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) && !report.faults);
  for (unsigned i = 0; i < 2; ++i)
    assert(
        !ps3_original_count(7 + i, 1, 31, PS3_ORIGINAL_INTERPRETER, &total) &&
        total == 2);
  puts("aggregate exceeds per-worker counter limit without changing per-worker "
       "semantics");
}
static void lifetime_history(void) {
  enum { LIFETIMES = 100513 };
  static uint64_t ids[LIFETIMES], generations[LIFETIMES];
  unsigned witnessed = 0;
  for (unsigned i = 0; i < LIFETIMES; ++i) {
    if (i & 1u) {
      assert(ps3_original_worker_prepare(&ids[i]) == 1);
      generations[i] = 1;
    } else {
      ids[i] = 100 + i / 4;
      generations[i] = 1 + (i / 2) % 2;
      int status = ps3_original_enroll(ids[i], generations[i]);
      if (status)
        fprintf(stderr, "lifetime history enrollment failed at %u status=%d\n",
                i, status);
      assert(!status);
    }
    if (i % 251 == 0) {
      /* Cancel only a genuinely never-started generation1 enrollment. */
      assert(generations[i] == 1 || !(i & 1u));
      if (generations[i] == 1) {
        assert(!ps3_original_worker_cancel(ids[i]));
        continue;
      }
    }
    current_test_worker = ids[i];
    current_test_generation = generations[i];
    witness_enabled = i % 257 == 0;
    witnessed += (unsigned)witness_enabled;
    run_new_worker();
  }
  ps3_original_live_report live = {.struct_size = sizeof(live),
                                   .abi_version = 1};
  uint64_t total = 0;
  assert(!ps3_original_live_status(&live) && live.workers == LIFETIMES &&
         !live.faults);
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_MICROSTEP, &total) &&
         total == witnessed);
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) && !report.faults &&
         report.workers == LIFETIMES);
  for (unsigned i = 0; i < LIFETIMES; ++i) {
    const uint64_t expected =
        i % 257 == 0 && !(i % 251 == 0 && generations[i] == 1);
    assert(!ps3_original_count(ids[i], generations[i], 31,
                               PS3_ORIGINAL_MICROSTEP, &total) &&
           total == expected);
    assert(!ps3_original_count(ids[i], generations[i], 31, PS3_ORIGINAL_OUTSIDE,
                               &total) &&
           total == expected);
    assert(!ps3_original_count(ids[i], generations[i], 31, PS3_ORIGINAL_LIFTED,
                               &total) &&
           total == 0);
  }
  assert(ps3_original_count(99, 1, 31, PS3_ORIGINAL_MICROSTEP, &total) == -1);
  puts("original observer100513 lifetimes (>100000 real threads) preserve "
       "sparse and zero history across slot reuse");
}
static void cancel_active(void) {
  uint64_t cancelled = 0, worker = 0, n = 0;
  assert(ps3_original_worker_prepare(&cancelled) == 1);
  assert(!ps3_original_worker_cancel(cancelled));
  assert(ps3_original_worker_cancel(cancelled) == -1);
  assert(ps3_original_worker_prepare(&worker) == 1);
  assert(!ps3_original_worker_register(worker, 1));
  PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_LIFTED);
  assert(ps3_original_worker_cancel(worker) == -1);
  PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_LIFTED);
  assert(!ps3_original_worker_retire());
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(!ps3_original_seal(&report) &&
         report.faults == PS3_ORIGINAL_MISSING_WORKER);
  assert(!ps3_original_count(cancelled, 1, 31, PS3_ORIGINAL_LIFTED, &n) &&
         n == 0);
  assert(!ps3_original_count(worker, 1, 31, PS3_ORIGINAL_LIFTED, &n) && n == 2);
  puts("native cancellation cannot replay or cancel an active worker; both "
       "histories retained");
}
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "good";
  ps3_original_source source = {
      .id = 11, .generation = 2, .provider_mask = 1, .code_sha256 = {1}};
  ps3_original_provider provider = {
      .id = 13, .route_mask = 63, .source_sha256 = {2}};
  ps3_original_target target = {
      .id = 31, .source = 11, .low = 16, .high = 32, .route_mask = 63};
  ps3_original_target many_targets[PS3_ORIGINAL_MAX_TARGETS];
  ps3_original_config config = {.struct_size = sizeof(config),
                                .abi_version = 1,
                                .source_count = 1,
                                .provider_count = 1,
                                .target_count = 1,
                                .covered_from = 100,
                                .counter_limit = UINT64_MAX,
                                .manifest_sha256 = {3},
                                .sources = &source,
                                .providers = &provider,
                                .targets = &target,
                                .resolve = resolve};
  if (!strcmp(mode, "arm_race")) {
    arm_racing_worker(&config);
    return 0;
  }
  if (!strcmp(mode, "late_arm")) {
    assert(ps3_original_worker_register(7, 3) == -1);
    assert(ps3_original_arm(&config) == -1);
    puts("original observer rejects late arming");
    return 0;
  }
  if (!strcmp(mode, "archive_budget")) {
    for (unsigned i = 0; i < PS3_ORIGINAL_MAX_TARGETS; ++i) {
      many_targets[i] = target;
      many_targets[i].id = 31 + i;
    }
    config.targets = many_targets;
    config.target_count = PS3_ORIGINAL_MAX_TARGETS;
  }
  if (!strcmp(mode, "overflow") || !strcmp(mode, "aggregate_limit"))
    config.counter_limit = 2;
  assert(ps3_original_arm(&config) == 0);
  if (!strcmp(mode, "lifetime_history")) {
    lifetime_history();
    return 0;
  }
  if (!strcmp(mode, "reused_tls")) {
    reused_tls();
    return 0;
  }
  if (!strcmp(mode, "replay_generation")) {
    replay_generation();
    return 0;
  }
  if (!strcmp(mode, "history_budget")) {
    history_budget();
    return 0;
  }
  if (!strcmp(mode, "archive_budget")) {
    archive_budget();
    return 0;
  }
  if (!strcmp(mode, "aggregate_limit")) {
    aggregate_limit();
    return 0;
  }
  if (!strcmp(mode, "cancel_active")) {
    cancel_active();
    return 0;
  }
  if (!strcmp(mode, "lifetime_capacity")) {
    lifetime_capacity();
    return 0;
  }
  /* Caller mutation cannot change a copied manifest. */
  source.generation = 99;
  target.low = 1000;
  config.covered_from = 9000;
  assert(ps3_original_enroll(7, 3) == 0);
  assert(ps3_original_worker_register(7, 3) == 0);
  assert(ps3_original_transaction_begin(71) == 0);
  errno = EDOM;
  PS3_ORIGINAL_STEP(&identity, 16, PS3_ORIGINAL_LIFTED);
  assert(errno == EDOM);
  assert(ps3_original_transaction_end(71) == 0);
  PS3_ORIGINAL_STEP(&identity, 20, PS3_ORIGINAL_INTERPRETER);
  assert(ps3_original_enroll(8, 1) == 0);
  run_new_worker();
  unsigned expected = 0;
  if (!strcmp(mode, "wrong_source")) {
    ++identity.generation;
    PS3_ORIGINAL_STEP(&identity, 16, PS3_ORIGINAL_LIFTED);
    expected = PS3_ORIGINAL_BAD_SOURCE;
  } else if (!strcmp(mode, "missing_worker")) {
    assert(ps3_original_enroll(9, 1) == 0);
    expected = PS3_ORIGINAL_MISSING_WORKER;
  } else if (!strcmp(mode, "overflow")) {
    PS3_ORIGINAL_STEP(&identity, 20, PS3_ORIGINAL_INTERPRETER);
    PS3_ORIGINAL_STEP(&identity, 20, PS3_ORIGINAL_INTERPRETER);
    expected = PS3_ORIGINAL_OVERFLOW;
  } else if (!strcmp(mode, "loss")) {
    ps3_original_lost(1);
    expected = PS3_ORIGINAL_LOSS;
  } else if (!strcmp(mode, "route")) {
    PS3_ORIGINAL_STEP(&identity, 20, PS3_ORIGINAL_OUTSIDE);
    expected = PS3_ORIGINAL_BAD_ROUTE;
  } else if (!strcmp(mode, "unregistered")) {
    rogue = 1;
    run_new_worker();
    expected = PS3_ORIGINAL_MISSING_WORKER;
  } else if (!strcmp(mode, "after_retire")) {
    expected = PS3_ORIGINAL_LATE_WORK;
  } else
    assert(!strcmp(mode, "good"));
  identity.generation = 2;
  assert(ps3_original_begin_drain(150) == 0);
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(ps3_original_seal(&report) ==
         -4); // Current worker still genuinely active.
  PS3_ORIGINAL_STEP(&identity, 28,
                    PS3_ORIGINAL_LEAF); // Late work remains visible.
  assert(ps3_original_worker_retire() == 0);
  if (!strcmp(mode, "after_retire"))
    PS3_ORIGINAL_STEP(&identity, 28, PS3_ORIGINAL_LEAF);
  assert(ps3_original_seal(&report) == 0);
  assert(report.faults == expected && report.covered_from == 100 &&
         report.drained_through == 150);
  assert(report.workers == (!strcmp(mode, "missing_worker") ? 3u : 2u));
  uint64_t n = 999;
  assert(ps3_original_count(7, 3, 31, PS3_ORIGINAL_LIFTED, &n) == 0 && n == 1);
  assert(ps3_original_count(8, 1, 31, PS3_ORIGINAL_MICROSTEP, &n) == 0 &&
         n == 1);
  assert(ps3_original_count(7, 3, 31, PS3_ORIGINAL_OUTSIDE, &n) == 0 && n >= 2);
  assert(ps3_original_count(7, 4, 31, PS3_ORIGINAL_LIFTED, &n) == -1);
  printf(
      "original observer mode=%s faults=%u workers=%u late_witness_preserved\n",
      mode, report.faults, report.workers);
}
