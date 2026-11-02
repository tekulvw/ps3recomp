/* Mid-function PPU target: entry-only sampling cannot detect this range. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "original_work_observer.h"
#include "ppu_recomp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t memory[65536];
uint8_t *vm_base = memory;
static int resolve(const void *c, uint64_t pc, unsigned route,
                   const char *provider, ps3_original_identity *out) {
  (void)c;
  (void)pc;
  assert(route == PS3_ORIGINAL_PPU && provider && *provider);
  *out = (ps3_original_identity){11, 2, 13};
  return 0;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  int observe = !strcmp(argv[1], "1");
  if (observe) {
    ps3_original_source source = {
        .id = 11, .generation = 2, .provider_mask = 1, .code_sha256 = {1}};
    ps3_original_provider provider = {
        .id = 13, .route_mask = 1, .source_sha256 = {2}};
    ps3_original_target target = {
        .id = 31, .source = 11, .low = 0x1004, .high = 0x1008, .route_mask = 1};
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
    assert(ps3_original_arm(&config) == 0);
    assert(ps3_original_enroll(7, 3) == 0 &&
           ps3_original_worker_register(7, 3) == 0);
  }
  ppu_context context = {0};
  ppu_observer_func_00001000(&context);
  assert(context.gpr[3] == 7);
  if (observe) {
    assert(ps3_original_begin_drain(150) == 0 &&
           ps3_original_worker_retire() == 0);
    ps3_original_report report = {.struct_size = sizeof(report),
                                  .abi_version = 1};
    assert(ps3_original_seal(&report) == 0 && report.faults == 0);
    uint64_t count = 0;
    assert(ps3_original_count(7, 3, 31, PS3_ORIGINAL_PPU, &count) == 0 &&
           count == 1);
  }
  puts("PPU_GUEST7 original_midfunction_observer_preserves_result");
}
