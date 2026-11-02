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
static int racing;
static atomic_int race_start;
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
  if (racing) {
    while (!atomic_load_explicit(&race_start, memory_order_acquire)) {
    }
    assert(ps3_original_worker_register(999, 1) == -1);
    for (unsigned i = 0; i < 10000; ++i)
      (void)ps3_original_begin_drain(150);
    return;
  }
  if (!rogue)
    assert(ps3_original_worker_register(8, 1) == 0);
  PS3_ORIGINAL_STEP(&identity, 24, PS3_ORIGINAL_MICROSTEP);
  if (!rogue)
    assert(ps3_original_worker_retire() == 0);
}
#ifdef _WIN32
static DWORD WINAPI new_worker(void *arg) {
  (void)arg;
  new_worker_body();
  return 0;
}
static void run_new_worker(void) {
  HANDLE thread = CreateThread(NULL, 0, new_worker, NULL, 0, NULL);
  assert(thread);
  assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  assert(CloseHandle(thread));
}
#else
static void *new_worker(void *arg) {
  (void)arg;
  new_worker_body();
  return NULL;
}
static void run_new_worker(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, new_worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
}
#endif
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
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "good";
  ps3_original_source source = {
      .id = 11, .generation = 2, .provider_mask = 1, .code_sha256 = {1}};
  ps3_original_provider provider = {
      .id = 13, .route_mask = 63, .source_sha256 = {2}};
  ps3_original_target target = {
      .id = 31, .source = 11, .low = 16, .high = 32, .route_mask = 63};
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
  if (!strcmp(mode, "overflow"))
    config.counter_limit = 2;
  assert(ps3_original_arm(&config) == 0);
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
