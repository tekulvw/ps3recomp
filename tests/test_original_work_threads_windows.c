#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../runtime/platform/win32_compat.h"
#include "original_work_observer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static HANDLE seen, go;
static int direct_exit, crt;
static ps3_original_identity identity = {11, 2, 13};
static int resolve(const void *context, uint64_t pc, unsigned route,
                   const char *name, ps3_original_identity *out) {
  (void)pc;
  (void)route;
  assert(name);
  *out = *(const ps3_original_identity *)context;
  return 0;
}
static DWORD WINAPI worker(void *arg) {
  assert(arg == &identity);
  PS3_ORIGINAL_STEP(arg, 16, PS3_ORIGINAL_LIFTED);
  assert(SetEvent(seen));
  assert(WaitForSingleObject(go, 5000) == WAIT_OBJECT_0);
  PS3_ORIGINAL_STEP(arg, 20, PS3_ORIGINAL_LIFTED);
  if (direct_exit) {
    if (crt)
      _endthreadex(0x77);
    ExitThread(0x77);
  }
  return 0x77;
}
static unsigned __stdcall crt_worker(void *arg) {
  return (unsigned)worker(arg);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  crt = !strncmp(argv[1], "crt", 3);
  direct_exit = strstr(argv[1], "exit") != NULL;
  ps3_original_source source = {
      .id = 11, .generation = 2, .provider_mask = 1, .code_sha256 = {1}};
  ps3_original_provider provider = {
      .id = 13, .route_mask = 2, .source_sha256 = {2}};
  ps3_original_target target = {
      .id = 31, .source = 11, .low = 16, .high = 32, .route_mask = 2};
  ps3_original_config config = {
      .struct_size = sizeof(config), .abi_version = 1, .source_count = 1,
      .provider_count = 1, .target_count = 1, .covered_from = 100,
      .counter_limit = UINT64_MAX, .manifest_sha256 = {3}, .sources = &source,
      .providers = &provider, .targets = &target, .resolve = resolve};
  assert(!ps3_original_arm(&config));
  seen = CreateEventA(NULL, TRUE, FALSE, NULL);
  go = CreateEventA(NULL, TRUE, FALSE, NULL);
  assert(seen && go);
  /* Preserve genuine suspended creation and real Windows/CRT stack flags.
   * There can be no original witness until the host explicitly resumes it. */
  HANDLE thread = crt ? (HANDLE)_beginthreadex(NULL, 1u << 20, crt_worker,
      &identity, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL)
      : CreateThread(NULL, 1u << 20, worker, &identity,
          CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  assert(thread);
  uint64_t count = 99;
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_LIFTED, &count) && count == 0);
  assert(WaitForSingleObject(seen, 0) == WAIT_TIMEOUT);
  assert(ResumeThread(thread) == 1);
  assert(WaitForSingleObject(seen, 5000) == WAIT_OBJECT_0);
  ps3_original_live_report live = {.struct_size = sizeof(live), .abi_version = 1};
  assert(!ps3_original_live_status(&live) && live.workers == 1 && !live.faults);
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_LIFTED, &count) && count == 1);
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report = {.struct_size = sizeof(report), .abi_version = 1};
  assert(ps3_original_seal(&report) == -4);
  assert(SetEvent(go));
  assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  DWORD result = 0;
  assert(GetExitCodeThread(thread, &result) && result == 0x77);
  assert(!ps3_original_seal(&report) && !report.faults);
  assert(!ps3_original_live_total(31, PS3_ORIGINAL_LIFTED, &count) && count == 2);
  assert(CloseHandle(thread) && CloseHandle(seen) && CloseHandle(go));
  printf("REAL_WINDOWS_THREAD mode=%s suspended_then_live1_late2_joined_retired\n", argv[1]);
}
