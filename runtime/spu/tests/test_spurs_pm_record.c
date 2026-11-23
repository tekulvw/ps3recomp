/* Policy-run recorder fixture (tools/test_spurs_pm_record.py).
 *
 * A manufactured policy module, run through the real policy runner, MFC path,
 * resolver and drain, performs each kind of event the recorder captures once:
 * GETLLAR, a successful and a failed PUTLLC, plain GETs of a header array and
 * of a job record, a PUT, a two-element GETL, PUTLLUC, a select-workload poll,
 * a call into manufactured "job" code that calls back into the module through
 * a slot of the context it was handed, a job PUT on tag 31, and the exit to
 * the kernel. Every guest structure is generated here; no title data.
 *
 * Quadword loads (spu_ls_read128, the lifted lqd/lqx/lqa) exercise the
 * local-store read events (PS3RECOMP_SPURS_PM_RECORD_LSREAD): the job reads the
 * block at its r3 (at and inside it), an absolute address below 0x4000, and
 * one above; the service and the policy code each read one quadword too.
 *
 * Prints the observed results and an FNV hash of guest memory so the script
 * can require identical behaviour with the recorder off, on, and statistics
 * only. argv[1] = runs. */
#include "spu_helpers.h"
#include "spu_workload.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

enum { MEM = 1u << 20, SPURS_EA = 0x10000, LIST_EA = 0x20000, HDR_EA = 0x21000, REC_EA = 0x22000,
       PUT_EA = 0x23000, GATHER_EA = 0x24000, UNCOND_EA = 0x25000, SVC_EA = 0x26000, JOB_PUT_EA = 0x27000,
       IMAGE = 77, PM_SIZE = 0x2F00,
       /* local store */
       LS_LINE = 0x1000, LS_HDR = 0x1100, LS_REC = 0x1180, LS_PUT = 0x1200, LS_LIST = 0x1280,
       LS_GATHER = 0x1300, LS_UNCOND = 0x1380, LS_CTX = 0x1400, LS_SVC = 0x1500, LS_JOB = 0x1600,
       PM_SERVICE = 0x1A00, PM_CALL_SITE = 0x2000, JOB_ENTRY = 0x4100, JOB_CALL_SITE = 0x4200, JOB2_ENTRY = 0x4400,
       SLOT = 0x44, SERVICE_RESULT = 0x5EC0FFEE };

static unsigned char memory[MEM];
uint8_t* vm_base = memory;
uint32_t ppu_vm_size = MEM;
extern unsigned char g_vm_page_bitmap[];
extern void spu_halt(spu_context*);
extern void spu_wrch(spu_context*, unsigned, u128);
extern u128 spu_rdch(spu_context*, unsigned);
extern void spu_indirect_branch(spu_context*);
extern void spu_drain_call(spu_context*, uint32_t);
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));

static uint32_t st_lsread_sum;
static uint32_t st_putllc_ok, st_putllc_fail, st_service_r3, st_job_saw, st_select_wid, st_observer;

static void mfc(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    spu_wrch(c, MFC_LSA, spu_splat_u32(lsa));
    spu_wrch(c, MFC_EAH, spu_splat_u32(0));
    spu_wrch(c, MFC_EAL, spu_splat_u32(ea));
    spu_wrch(c, MFC_Size, spu_splat_u32(size));
    spu_wrch(c, MFC_TagID, spu_splat_u32(tag));
    spu_wrch(c, MFC_Cmd, spu_splat_u32(cmd));
}
static uint32_t atomic_stat(spu_context* c) { return spu_rdch(c, MFC_RdAtomicStat)._u32[0]; }

/* The service the job calls back: one GET, a result in r3, return to r0. */
static void service(spu_context* c)
{
    st_lsread_sum += spu_ls_read128(c, 0x1C80)._u32[1];   /* a service read: depth 1 */
    mfc(c, 0x40, LS_SVC, SVC_EA, 0x20, 3);
    c->gpr[3] = spu_splat_u32(SERVICE_RESULT + c->gpr[4]._u32[0]);
    SPU_RET(c);
}

/* Job code: calls the service through the slot of the context in r3 (the
 * lifter's bisl bracket), PUTs on tag 31, returns to r0. */
static void job(spu_context* c)
{
    const uint32_t ctx_ls = c->gpr[3]._u32[0];
    st_lsread_sum += spu_ls_read128(c, ctx_ls)._u32[0];          /* at r3        */
    st_lsread_sum += spu_ls_read128(c, ctx_ls + 0x14)._u32[1];   /* inside r3's block */
    st_lsread_sum += spu_ls_read128(c, 0x1C48)._u32[2];          /* absolute, low */
    st_lsread_sum += spu_ls_read128(c, 0x5000)._u32[3];          /* above the limit */
    const uint32_t target = spu_ls_read32(c, ctx_ls + SLOT);
    const u128 link = c->gpr[0];
    uint32_t _tg = target; c->gpr[0] = spu_link(JOB_CALL_SITE + 4); int32_t _si = (int32_t)c->image_id;
    c->gpr[3] = spu_splat_u32(0); c->gpr[4] = spu_splat_u32(1); c->gpr[5] = spu_splat_u32(0);
    c->gpr[6] = spu_splat_u32(1);
    c->pc = _tg; c->host_depth++;
    spu_indirect_branch(c); spu_drain_call(c, JOB_CALL_SITE + 4);
    c->host_depth--; spu_img_restore(c, _si);
    st_service_r3 = c->gpr[3]._u32[0];
    st_job_saw = spu_ls_read32(c, LS_SVC);
    spu_ls_write32(c, LS_JOB, st_service_r3);
    mfc(c, 0x20, LS_JOB, JOB_PUT_EA, 0x10, 31);
    c->gpr[0] = link;
    SPU_RET(c);
}

/* A second job: returns at once (`bi $r0`). */
static void job2(spu_context* c)
{
    /* PMREC_FIXTURE_HALT: the second job halts (a conditional-halt assert),
     * with a marker in a high register, for the register-file events */
    if (getenv("PMREC_FIXTURE_HALT")) {
        c->gpr[77]._u32[2] = 0xFEEDu;
        c->status = SPU_STATUS_STOPPED_BY_HALT;
        spu_halt(c);
        return;
    }
    SPU_RET(c);
}

static void policy(spu_context* c)
{
    st_observer = c->pm_observer != NULL;
    for (uint32_t o = 0; o < 0x100; o += 4) spu_ls_write32(c, 0x1C00 + o, 0xC0FFEE00u + o);
    spu_ls_write32(c, 0x5000 + 12, 0x0BADF00Du);
    st_lsread_sum += spu_ls_read128(c, 0x1C00)._u32[0];   /* a policy-region read */
    /* claim-style line transaction: GETLLAR, PUTLLC (success), PUTLLC again (no reservation) */
    mfc(c, MFC_GETLLAR_CMD, LS_LINE, LIST_EA, 128, 0); (void)atomic_stat(c);
    spu_ls_write32(c, LS_LINE + 0x40, spu_ls_read32(c, LS_LINE + 0x40) + 1);
    mfc(c, MFC_PUTLLC_CMD, LS_LINE, LIST_EA, 128, 0); st_putllc_ok += atomic_stat(c) == 0;
    mfc(c, MFC_PUTLLC_CMD, LS_LINE, LIST_EA, 128, 0); st_putllc_fail += atomic_stat(c) != 0;
    /* header array and the record's command portion */
    mfc(c, 0x40, LS_HDR, HDR_EA, 0x10, 1);
    mfc(c, 0x40, LS_REC, REC_EA, 0x40, 1);
    /* a plain store and a two-element gather */
    spu_ls_write32(c, LS_PUT, 0xA11CE000u + spu_ls_read32(c, LS_REC));
    mfc(c, 0x20, LS_PUT, PUT_EA, 0x10, 5);
    spu_ls_write32(c, LS_LIST + 0, 0x20); spu_ls_write32(c, LS_LIST + 4, GATHER_EA);
    spu_ls_write32(c, LS_LIST + 8, 0x10); spu_ls_write32(c, LS_LIST + 12, GATHER_EA + 0x100);
    mfc(c, 0x44, LS_GATHER, LS_LIST, 16, 6);
    /* unconditional line store */
    memset(c->ls + LS_UNCOND, 0x5A, 128);
    mfc(c, MFC_PUTLLUC_CMD, LS_UNCOND, UNCOND_EA, 128, 0); (void)atomic_stat(c);
    /* kernel poll: bisl through selectWorkload */
    c->gpr[0] = spu_link(PM_CALL_SITE); c->pc = SPURS_PM_SELECT_WORKLOAD_LS;
    spu_indirect_branch(c);
    st_select_wid = c->gpr[3]._u32[0];
    assert((c->pc & SPU_LS_MASK) == PM_CALL_SITE);
    /* hand a job its context (service slot at +SLOT) and enter it: bisl bracket */
    memset(c->ls + LS_CTX, 0, 0x80);
    spu_ls_write32(c, LS_CTX + 0x00, 3);
    spu_ls_write32(c, LS_CTX + SLOT, PM_SERVICE);
    { uint32_t _tg = JOB_ENTRY; c->gpr[0] = spu_link(PM_CALL_SITE + 0x14); int32_t _si = (int32_t)c->image_id;
      c->gpr[1] = spu_splat_u32(0x3FF00); c->gpr[3] = spu_splat_u32(LS_CTX); c->gpr[4] = spu_splat_u32(0);
      c->pc = _tg; c->host_depth++;
      spu_indirect_branch(c); spu_drain_call(c, PM_CALL_SITE + 0x14);
      c->host_depth--; spu_img_restore(c, _si); }
    /* and a second job from the same call depth */
    { c->gpr[0] = spu_link(PM_CALL_SITE + 0x18); int32_t _si = (int32_t)c->image_id;
      c->gpr[3] = spu_splat_u32(LS_CTX); c->pc = JOB2_ENTRY; c->host_depth++;
      spu_indirect_branch(c); spu_drain_call(c, PM_CALL_SITE + 0x18);
      c->host_depth--; spu_img_restore(c, _si); }
    /* exitToKernel */
    c->gpr[0] = spu_link(PM_CALL_SITE + 0x20); c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
    spu_indirect_branch(c);
    abort();   /* exit-to-kernel halts */
}

static uint64_t fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static void be32(uint32_t ea, uint32_t v)
{ memory[ea] = v >> 24; memory[ea + 1] = v >> 16; memory[ea + 2] = v >> 8; memory[ea + 3] = v; }

int main(int argc, char** argv)
{
    const int runs = argc > 1 ? atoi(argv[1]) : 2;
    static uint8_t pm[PM_SIZE];
    memset(memory, 0, sizeof(memory));
    memset(g_vm_page_bitmap, 0, 8192);
    g_vm_page_bitmap[0] = 0xFF; g_vm_page_bitmap[1] = 0xFF;
    for (unsigned i = 0; i < PM_SIZE; i++) pm[i] = (uint8_t)(i * 7u);
    /* the workload's line: one header array of 8 entries at HDR_EA */
    be32(LIST_EA + 0x10, 1); be32(LIST_EA + 0x20, 7); be32(LIST_EA + 0x30, HDR_EA);
    memory[LIST_EA + 0x40] = 0;
    /* header 0: job (type 1), 0x40 command bytes at REC_EA; header 1: no-op (type 3) */
    be32(HDR_EA + 0, 0x00400001u); be32(HDR_EA + 4, REC_EA);
    be32(HDR_EA + 8, 0x00000003u); be32(HDR_EA + 12, 0);
    /* commands: reserve, use (read), use (read+gather), run, end, nop, nop, nop */
    const uint32_t cmd[8] = {
        (1u << 9) | (0u << 5),
        (0x700u << 18) | (1u << 15) | (1u << 13) | (2u << 9) | (0u << 5) | 0u,
        (12u << 18) | (1u << 15) | (1u << 14) | (2u << 9) | (1u << 5) | 1u,
        (5u << 9), (6u << 9), 0, 0, 0 };
    for (int i = 0; i < 8; i++) { be32(REC_EA + 8 * i, cmd[i]); be32(REC_EA + 8 * i + 4, 0x1000u * i); }
    for (unsigned i = 0; i < 0x200; i++) memory[GATHER_EA + i] = (uint8_t)(0x30 + i);
    for (unsigned i = 0; i < 0x20; i++) memory[SVC_EA + i] = (uint8_t)(0x90 + i);
    spu_begin_image(IMAGE);
    spu_register_function(PM_SERVICE, service);
    spu_register_function(JOB_ENTRY, job);
    spu_register_function(JOB2_ENTRY, job2);
    /* Another SPU thread has already settled the read-diagnostic gate to "none
     * armed" before the first policy run, as in a title. */
    g_spu_ls_rdiag = 0;
    int rc = 0;
    for (int r = 0; r < runs; r++)
        rc |= spu_run_policy_module(policy, IMAGE, pm, sizeof(pm), LIST_EA, 6, SPURS_EA, 2);
    printf("rc=%d putllc_ok=%u putllc_fail=%u service_r3=%08X job_saw=%08X select_wid=%u observer=%u "
           "lsread_sum=%08X pmread=%d rdiag=%d mem=%016llX\n",
           rc, st_putllc_ok, st_putllc_fail, st_service_r3, st_job_saw, st_select_wid, st_observer,
           st_lsread_sum, g_spu_ls_pmread != NULL, g_spu_ls_rdiag, (unsigned long long)fnv(memory, sizeof(memory)));
    return rc ? 1 : 0;
}

/* External title/PPU boundaries; no emulated policy or DMA behavior is mocked. */
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
