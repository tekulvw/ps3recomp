/* Lifted-style call cost through the real drain and indirect dispatch.
 *
 * Hand-written stand-ins for what the lifter emits (no lifted code, no game
 * data): a caller runs N calls of an empty registered leaf through each
 * bracket the lifter uses -- bisl through spu_indirect_branch_mt (the
 * musttail fast path) and through the full resolver spu_indirect_branch, and
 * brsl (a direct call) -- each followed by spu_drain_call and
 * spu_img_restore, and the leaf returns with SPU_RET. Prints ns per call and
 * fails unless every call reached the leaf and came back. argv[1] = calls. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size = 0;
#include "../spu_channels.c"
#include "spu_helpers.h"
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
unsigned long long ps3_ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return 1000ull + t.tv_sec * 1000ull + t.tv_nsec / 1000000ull; }
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a; (void)b; }
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void ydkj_wake_all_event_flags(void) {}
void spu_indirect_branch_mt(spu_context* ctx);
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
enum { LEAF = 0x30000, CALLER = 0x20000 };
static volatile unsigned g_leaf_runs;
static void leaf(spu_context* ctx) { g_leaf_runs++; SPU_RET(ctx); }
static unsigned g_n; static double g_bisl, g_brsl, g_bisl_full;
static void caller(spu_context* ctx)
{
    double t0 = now_s();
    for (unsigned i = 0; i < g_n; i++) {   /* bisl $0,$9 (lifter's indirect call bracket) */
        uint32_t _tg = LEAF; ctx->gpr[0] = spu_link(CALLER + 0x10); int32_t _si = (int32_t)ctx->image_id;
        ctx->pc = _tg; ctx->host_depth++;
        spu_indirect_branch_mt(ctx); spu_drain_call(ctx, CALLER + 0x10);
        ctx->host_depth--; spu_img_restore(ctx, _si);
    }
    g_bisl = (now_s() - t0) * 1e9 / g_n;
    t0 = now_s();
    for (unsigned i = 0; i < g_n; i++) {   /* same through the full resolver */
        uint32_t _tg = LEAF; ctx->gpr[0] = spu_link(CALLER + 0x10); int32_t _si = (int32_t)ctx->image_id;
        ctx->pc = _tg; ctx->host_depth++;
        spu_indirect_branch(ctx); spu_drain_call(ctx, CALLER + 0x10);
        ctx->host_depth--; spu_img_restore(ctx, _si);
    }
    g_bisl_full = (now_s() - t0) * 1e9 / g_n;
    t0 = now_s();
    for (unsigned i = 0; i < g_n; i++) {   /* brsl $0,LEAF (direct call bracket) */
        ctx->gpr[0] = spu_link(CALLER + 0x20); int32_t _si = (int32_t)ctx->image_id;
        ctx->host_depth++; leaf(ctx); spu_drain_call(ctx, CALLER + 0x20);
        ctx->host_depth--; spu_img_restore(ctx, _si);
    }
    g_brsl = (now_s() - t0) * 1e9 / g_n;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(ctx);
}
int main(int argc, char** argv)
{
    g_n = argc > 1 ? (unsigned)atoi(argv[1]) : 5000000u;
    vm_base = calloc(1, 1 << 20);
    spu_begin_image(7);
    spu_register_function(LEAF, leaf);
    spu_register_function(CALLER, caller);
    spu_context* c = calloc(1, sizeof *c); spu_context_init(c, 0); c->image_id = 7;
    c->pc = CALLER; c->status = SPU_STATUS_RUNNING;
    spu_run_with_halt(caller, c);
    printf("bisl_mt_ns=%.1f bisl_full_ns=%.1f brsl_ns=%.1f leaf_runs=%u\n", g_bisl, g_bisl_full, g_brsl, g_leaf_runs);
    if (g_leaf_runs != 3u * g_n || c->host_depth != 0 || c->drain_ret_pc != 0) { fprintf(stderr, "FAIL: calls did not all return\n"); return 1; }
    return 0;
}
