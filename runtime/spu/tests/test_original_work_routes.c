/* Actual generated bodies + interpreter + microstep. Synthetic program only. */
#define main unused_mfc_fixture_main
#include "test_spu_mfc_slots.c"
#undef main
#include "observer_program.h"
#include "original_work_observer.h"
#include <assert.h>
void observer_spu_func_00010000(spu_context *);
void region_observer_spu_func_00010000(spu_context *);
void observer_spu_recomp_register(void);
void region_observer_spu_recomp_register(void);
int leaf_observer_spu_leaf_register(void);
void halt_observer_spu_func_00010000(spu_context *);
static const uint8_t *expected_program = observer_program;
static size_t expected_length = sizeof(observer_program);
static int resolve(const void *opaque, uint64_t pc, unsigned route,
                   const char *provider, ps3_original_identity *out) {
  const spu_context *c = opaque;
  (void)pc;
  (void)route;
  if (!provider || memcmp(c->ls + 0x10000, expected_program, expected_length))
    return -1;
  *out = (ps3_original_identity){11, 2, 13};
  return 0;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  int halt = !strcmp(argv[1], "halt");
  if (halt) {
    expected_program = observer_halt_program;
    expected_length = sizeof(observer_halt_program);
  }
  unsigned route = !strcmp(argv[1], "direct")   ? PS3_ORIGINAL_LIFTED
                   : !strcmp(argv[1], "interp") ? PS3_ORIGINAL_INTERPRETER
                   : !strcmp(argv[1], "micro")  ? PS3_ORIGINAL_MICROSTEP
                   : !strcmp(argv[1], "region") ? PS3_ORIGINAL_REGION
                   : halt                       ? PS3_ORIGINAL_LIFTED
                                                : PS3_ORIGINAL_LEAF;
  ps3_original_source source = {.id = 11, .generation = 2, .provider_mask = 1};
  memcpy(source.code_sha256,
         halt ? observer_halt_sha256 : observer_program_sha256, 32);
  ps3_original_provider provider = {
      .id = 13, .route_mask = 63, .source_sha256 = {2}};
  ps3_original_target target[2] = {{.id = 31,
                                    .source = 11,
                                    .low = halt ? 0x10004 : 0x10010,
                                    .high = halt ? 0x10008 : 0x10014,
                                    .route_mask = 63},
                                   {.id = 32,
                                    .source = 11,
                                    .low = 0x10008,
                                    .high = 0x1000c,
                                    .route_mask = 63}};
  ps3_original_config config = {.struct_size = sizeof(config),
                                .abi_version = 1,
                                .source_count = 1,
                                .provider_count = 1,
                                .target_count = halt ? 2 : 1,
                                .covered_from = 100,
                                .counter_limit = UINT64_MAX,
                                .manifest_sha256 = {3},
                                .sources = &source,
                                .providers = &provider,
                                .targets = target,
                                .resolve = resolve};
  assert(ps3_original_arm(&config) == 0);
  assert(ps3_original_enroll(7, 3) == 0 &&
         ps3_original_worker_register(7, 3) == 0);
  spu_context *c = calloc(1, sizeof(*c));
  assert(c);
  spu_context_init(c, 0);
  memcpy(c->ls + 0x10000, expected_program, expected_length);
  c->image_id = 501;
  c->policy_mode = 1;
  c->status = SPU_STATUS_RUNNING;
  c->gpr[0] = spu_link(0x20000);
  c->gpr[6] = spu_link(0x20000);
  c->gpr[7] = spu_splat_u32(1);
  spu_verified_code_segment segment = {0x10000, sizeof(observer_program),
                                       observer_program};
  assert(spu_verified_overlay_register(501, 701, &segment, 1));
  spu_begin_image(701);
  if (route == PS3_ORIGINAL_REGION)
    region_observer_spu_recomp_register();
  else
    observer_spu_recomp_register();
  spu_begin_image(0);
  ++c->host_depth;
  if (halt) {
    c->pc = 0x10000;
    halt_observer_spu_func_00010000(c);
  } else if (route == PS3_ORIGINAL_INTERPRETER) {
    c->policy_mode = 0;
    assert(spu_interp_run_until(c, 0x10000, 0x20000) == 0x20000);
  } else if (route == PS3_ORIGINAL_MICROSTEP) {
    c->pc = 0x10010;
    assert(spu_smc_microstep(c) == 1);
  } else if (route == PS3_ORIGINAL_LEAF) {
    assert(leaf_observer_spu_leaf_register());
    c->pc = 0x10010;
    spu_verified_leaf_indirect_call(c, 0x20000);
    spu_drain_call(c, 0x20000);
  } else {
    c->pc = 0x10000;
    if (route == PS3_ORIGINAL_REGION)
      region_observer_spu_func_00010000(c);
    else
      observer_spu_func_00010000(c);
    spu_drain_call(c, 0x20000);
  }
  --c->host_depth;
  assert(c->pc == (halt ? 0x10000u : 0x20000u));
  assert(c->gpr[7]._u32[0] ==
         (halt ? 1u
          : route == PS3_ORIGINAL_LEAF || route == PS3_ORIGINAL_MICROSTEP
              ? 3u
              : 4u));
  assert(ps3_original_begin_drain(150) == 0 &&
         ps3_original_worker_retire() == 0);
  ps3_original_report report = {.struct_size = sizeof(report),
                                .abi_version = 1};
  assert(ps3_original_seal(&report) == 0 && report.faults == 0);
  uint64_t count = 0;
  assert(ps3_original_count(7, 3, 31, route, &count) == 0);
  if (!(halt ? count == 0 : count > 0)) {
    fprintf(
        stderr,
        "ORIGINAL_WITNESS_MISMATCH route=%s guest_result_already_verified\n",
        argv[1]);
    spu_mfc_release(c);
    spu_coh_unregister(c);
    free(c);
    return 42;
  }
  assert(ps3_original_count(7, 3, 31, PS3_ORIGINAL_OUTSIDE, &count) == 0 &&
         (halt ? count == 0 : count > 0));
  if (halt)
    assert(ps3_original_count(7, 3, 32, route, &count) == 0 && count == 1);
  spu_mfc_release(c);
  spu_coh_unregister(c);
  free(c);
  printf("ORIGINAL_ROUTE %s %s guest_result_unchanged\n", argv[1],
         halt ? "skipped_range_zero_reachable_branch_detected"
              : "targeted_original_detected");
}
