/* The native WWS job manager's run loop (runtime/spu/wwsjm/wwsjm_run.inc)
 * through the real SPURS initializer, coordinator, five workers, policy
 * runner, resolver, MFC engine and drain. Manufactured job list, records,
 * modules, inputs and job code only; no title data.
 *
 * A PPU-side stand-in publishes a job list on two header arrays (the second
 * with a capacity that is not a power of two, its indices crossing the slot
 * wrap): a no-op, jobs on two modules, a gate opened by the jobs' dependency
 * decrements and their wake, a report gate, an array switch. Every job checks
 * what the specification says it is handed (registers, context words,
 * parameter data, module bytes, gathered inputs, get-buffer descriptors and
 * tags), writes an output with a DMA PUT, releases, calls finish and returns.
 * A monitor thread checks the PPU's completion predicate against the outputs
 * while the lanes run.
 *
 * Each job also checks the fixed low local store (spec 13.4): the interrupt
 * vector quadword at 0, zeros to 0x7F, the kernel context fields at 0x1C0-0x1E7,
 * the context at 0x1330 and the command copy at 0xC00; then it uses all of its
 * scratch 0x80-0x17F (a GETLLAR of its list line into 0x80, as the jobs do, and
 * a fill of the rest) before calling any service, so the manager must keep
 * nothing there while a job runs. After its last service call it also leaves
 * junk at 0x10-0x7F and 0x1C0-0x1E7, so the next job sees the fixed contents
 * only if the manager plants them before every job. It checks that it was
 * entered with SPU interrupts enabled and no event enabled (spec 5.1), runs
 * its service calls with interrupts disabled as the jobs do (and checks no
 * service changed that), and returns with interrupts disabled and an event
 * enabled, so the next job sees the required state only if the manager sets
 * it before every job.
 *
 * argv[1]: "native" (register the implementation for the manufactured image),
 * "native-irq" (the same, and the first job branches to the interrupt vector's
 * target: the process must stop loudly) or "unset" (register nothing extra;
 * the switch decides). Prints one
 * "result" line of key=value fields. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
#include "cellSpurs.c"
#include "spurs_native_pm.h"
#include "spu_helpers.h"
void spu_indirect_branch(spu_context*);
void spu_drain_call(spu_context*, uint32_t);
void spu_wrch(spu_context*, uint32_t, u128);
u128 spu_rdch(spu_context*, uint32_t);
void spu_begin_image(int);
void spu_register_function(uint32_t, void (*)(spu_context*));
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000ull + (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

enum {
    GUEST = 32u << 20, SPURS_EA = 0x100000, PM_EA = 0x200000, PM_SIZE = 0x200, LIST_EA = 0x300000,
    ARR0_EA = 0x310000, ARR1_EA = 0x320000, REC_EA = 0x400000, GLIST_EA = 0x500000, IN_EA = 0x600000,
    OUT_EA = 0x700000, MOD_A_EA = 0x800000, MOD_B_EA = 0x810000, MOD_BYTES = 0x200,
    GATE_EA = 0x900004, REPORT_EA = 0x900104, LIST2_EA = 0x330000, ARR2_EA = 0x340000,
    WID = 3, WID2 = 4, IMAGE = 61, NSPUS = 5,
    ENTRY_A = 0x30, ENTRY_B = 0x40, JOB_SITE = 0x4800, OUT_MAGIC = 0x0D0E0F01,
    MAX_JOBS = 64
};
/* array 0: nop, 20 jobs (the first 8 decrement the gate), gate, 20 jobs, switch.
 * array 1 starts at index 0x2C: report gate, 20 jobs (slots 0x2D..0x2F, 0..0x10). */
enum { A0_JOBS1 = 20, A0_DEPS = 8, A0_JOBS2 = 20, A1_START = 0x2C, A1_JOBS = 20, A1_CAP = 0x30 };

static struct { uint32_t array, index, module, sum, wid, list; } s_job[MAX_JOBS];
static unsigned s_jobs;
static volatile uint32_t g_runs[MAX_JOBS];
static volatile uint32_t g_err_regs, g_err_ctx, g_err_ud, g_err_module, g_err_gather, g_err_desc, g_err_tag,
    g_err_finish, g_violations, g_lifted_runs, g_finish_jobs, g_breaks, g_err_lowls, g_err_irq, g_err_stack;
static int g_irq_test;

/* ---- guest construction (the PPU stand-in) -------------------------------------- */
static void be32w(uint32_t ea, uint32_t v) { vm_write32(ea, v); }
static void be16w(uint32_t ea, uint16_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
static uint16_t be16r(uint32_t ea) { return (uint16_t)(vm_base[ea] << 8 | vm_base[ea + 1]); }
static uint32_t be32r(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void header(uint32_t array_ea, uint32_t cap, uint32_t index, uint16_t size, uint16_t type, uint32_t ea)
{
    const uint32_t at = array_ea + 8u * (index % cap);
    be16w(at, size); be16w(at + 2, type); be32w(at + 4, ea);
}

/* The MFC list rule, written independently of the core: each element keeps
 * its EA's quadword offset and the next starts on the quadword after its end. */
struct piece { uint32_t off, ea, size; };
static const struct { uint32_t off, size; } k_elems[4] = { {0x000, 0x40}, {0x048, 0x08}, {0x100, 0x30}, {0x184, 0x04} };
static unsigned layout(uint32_t in, struct piece* out)
{
    uint32_t at = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t ea = in + k_elems[i].off, lso = at + (ea & 15u);
        out[i].off = lso; out[i].ea = ea; out[i].size = k_elems[i].size;
        at = (lso + k_elems[i].size + 15u) & ~15u;
    }
    return 4;
}

static void module_image(uint32_t ea, uint32_t entry, uint8_t seed)
{
    for (uint32_t i = 0; i < MOD_BYTES; i++) vm_base[ea + i] = (uint8_t)(i * 7u + seed);
    be32w(ea + 0x10, entry); be32w(ea + 0x14, MOD_BYTES); be32w(ea + 0x20, 0x4D4F4421u); be32w(ea + 0x28, 0x4000);
}

static uint32_t record(unsigned k, int dep)
{
    const uint32_t rec = REC_EA + 0x100u * k, mod = s_job[k].module ? MOD_B_EA : MOD_A_EA;
    unsigned n = 0;
#define CMD(w0, w1) do { be32w(rec + 8 * n, (w0)); be32w(rec + 8 * n + 4, (w1)); n++; } while (0)
    CMD(0x00000200, 0x10010100);                               /* set 0: 1 page at 16      */
    CMD((MOD_BYTES / 16u) << 18 | 0xA400, mod);                /* read+shareable module    */
    CMD(0x00000220, 0x20020200);                               /* set 1: 2 x 2 pages at 32 */
    CMD(2u << 18 | 0xC420, GLIST_EA + 0x40u * k);              /* read+gather, 2 quadwords */
    CMD(0x00000260, 0x30010100);                               /* set 3: 1 page at 48      */
    CMD(0x00000460, 0);
    if (dep) CMD(0x00000800, GATE_EA);
    CMD(0x00000600, 0xF);
    CMD(0x00000A00, 0);
#undef CMD
    const uint32_t ud = (8u * n + 15u) & ~15u;
    be32w(rec + ud + 0x00, k); be32w(rec + ud + 0x04, OUT_EA + 0x10u * k); be32w(rec + ud + 0x08, s_job[k].sum);
    be32w(rec + ud + 0x0C, s_job[k].module); be32w(rec + ud + 0x10, s_job[k].array);
    be32w(rec + ud + 0x14, s_job[k].index); be32w(rec + ud + 0x18, (uint32_t)dep); be32w(rec + ud + 0x1C, 0x55D47A00u);
    /* the gather list and its inputs */
    const uint32_t in = IN_EA + 0x200u * k;
    for (uint32_t i = 0; i < 0x200; i++) vm_base[in + i] = (uint8_t)(i * 13u + k * 31u + 1u);
    struct piece pc[4];
    layout(in, pc);
    uint32_t sum = 0;
    for (unsigned i = 0; i < 4; i++) {
        be32w(GLIST_EA + 0x40u * k + 8u * i, pc[i].size);
        be32w(GLIST_EA + 0x40u * k + 8u * i + 4, pc[i].ea);
        for (uint32_t b = 0; b < pc[i].size; b++) sum += vm_base[pc[i].ea + b];
    }
    s_job[k].sum = sum;
    be32w(rec + ud + 0x08, sum);
    return ud + 0x20;   /* the header's size: commands and parameter data */
}

static unsigned add_job(uint32_t array, uint32_t index, int dep)
{
    const unsigned k = s_jobs++;
    s_job[k].array = array; s_job[k].index = index; s_job[k].module = (k / 2) & 1;
    s_job[k].wid = WID; s_job[k].list = LIST_EA;
    const uint32_t size = record(k, dep);
    if (array == 0) header(ARR0_EA, 64, index, (uint16_t)size, 1, REC_EA + 0x100u * k);
    else header(ARR1_EA, A1_CAP, index, (uint16_t)size, 1, REC_EA + 0x100u * k);
    return k;
}

/* The second workload: one job, started only by the gate counter's wake. */
static void build_list2(void)
{
    memset(vm_base + LIST2_EA, 0, 128);
    be32w(LIST2_EA + 0x00, PM_EA); be32w(LIST2_EA + 0x04, PM_SIZE); be16w(LIST2_EA + 0x0E, 0xFFFF);
    be32w(LIST2_EA + 0x10, 1); be32w(LIST2_EA + 0x20, 15); be32w(LIST2_EA + 0x30, ARR2_EA);
    for (unsigned a = 0; a < 4; a++) for (unsigned s = 0; s < 6; s++) be16w(LIST2_EA + 0x44 + 16 * a + 2 * s, 0xFFFF);
    const unsigned k = s_jobs++;
    s_job[k].array = 0; s_job[k].index = 0; s_job[k].module = 1; s_job[k].wid = WID2; s_job[k].list = LIST2_EA;
    const uint32_t size = record(k, 0);
    header(ARR2_EA, 16, 0, (uint16_t)size, 1, REC_EA + 0x100u * k);
    be16w(LIST2_EA + 0x40, 1);
}

static void build_list(void)
{
    memset(vm_base + LIST_EA, 0, 128);
    be32w(LIST_EA + 0x00, PM_EA); be32w(LIST_EA + 0x04, PM_SIZE);
    be16w(LIST_EA + 0x0A, 0); be16w(LIST_EA + 0x0E, 0xFFFF);
    be32w(LIST_EA + 0x10, 1); be32w(LIST_EA + 0x14, 1);
    be32w(LIST_EA + 0x20, 63); be32w(LIST_EA + 0x24, A1_CAP - 1);
    be32w(LIST_EA + 0x30, ARR0_EA); be32w(LIST_EA + 0x34, ARR1_EA);
    for (unsigned a = 0; a < 4; a++) for (unsigned s = 0; s < 6; s++) be16w(LIST_EA + 0x44 + 16 * a + 2 * s, 0xFFFF);
    module_image(MOD_A_EA, ENTRY_A, 0x11);
    module_image(MOD_B_EA, ENTRY_B, 0x77);
    be16w(GATE_EA, A0_DEPS); be16w(GATE_EA + 2, (uint16_t)(WID2 << 8 | 5));   /* wakes the second workload */
    be16w(REPORT_EA, 0);
    uint32_t i = 0;
    header(ARR0_EA, 64, i++, 0, 3, 0);                                  /* no-op  */
    for (unsigned j = 0; j < A0_JOBS1; j++) add_job(0, i++, j < A0_DEPS);
    header(ARR0_EA, 64, i++, 0, 2, GATE_EA);                            /* gate   */
    for (unsigned j = 0; j < A0_JOBS2; j++) add_job(0, i++, 0);
    header(ARR0_EA, 64, i++, 0, 4, 0);                                  /* switch */
    be16w(LIST_EA + 0x40, (uint16_t)i);                                 /* S0     */
    uint32_t k = A1_START;
    header(ARR1_EA, A1_CAP, k++, 0, 5, REPORT_EA);                      /* report gate */
    for (unsigned j = 0; j < A1_JOBS; j++) add_job(1, k++, 0);
    header(ARR1_EA, A1_CAP, k, 0, 0, 0);                                /* nothing more */
    be16w(LIST_EA + 0x50, (uint16_t)k);                                 /* S1 */
    be16w(LIST_EA + 0x52, A1_START);                                    /* N1 */
}

/* ---- job code (lifted-style C) ---------------------------------------------------- */
static uint32_t call_service(spu_context* c, uint32_t ctx_ls, uint32_t word, uint32_t r4, uint32_t r5, uint32_t r6,
                             uint32_t out[4])
{
    const uint32_t target = spu_ls_read32(c, ctx_ls + word);
    c->gpr[0] = spu_link(JOB_SITE + 4); int32_t si = (int32_t)c->image_id;
    c->gpr[3] = spu_make_preferred_u32(0); c->gpr[4] = spu_make_preferred_u32(r4);
    c->gpr[5] = spu_make_preferred_u32(r5); c->gpr[6] = spu_make_preferred_u32(r6);
    c->pc = target; c->host_depth++;
    spu_indirect_branch(c); spu_drain_call(c, JOB_SITE + 4);
    c->host_depth--; spu_img_restore(c, si);
    if ((c->pc & SPU_LS_MASK) != JOB_SITE + 4) __atomic_add_fetch(&g_err_regs, 1, __ATOMIC_ACQ_REL);
    if (out) for (int i = 0; i < 4; i++) out[i] = c->gpr[3]._u32[i];
    return c->gpr[3]._u32[0];
}
static void mfc(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    spu_wrch(c, MFC_LSA, spu_make_preferred_u32(lsa)); spu_wrch(c, MFC_EAH, spu_make_preferred_u32(0));
    spu_wrch(c, MFC_EAL, spu_make_preferred_u32(ea)); spu_wrch(c, MFC_Size, spu_make_preferred_u32(size));
    spu_wrch(c, MFC_TagID, spu_make_preferred_u32(tag)); spu_wrch(c, MFC_Cmd, spu_make_preferred_u32(cmd));
}
#define ERR(v) __atomic_add_fetch(&(v), 1, __ATOMIC_ACQ_REL)

static void job_common(spu_context* c, uint32_t module, uint32_t entry)
{
    const u128 link = c->gpr[0];
    const uint32_t ctx_ls = c->gpr[3]._u32[0];
    if (c->gpr[3]._u32[1] != ctx_ls || c->gpr[3]._u32[2] != ctx_ls || c->gpr[3]._u32[3] != ctx_ls ||
        c->gpr[4]._u32[0] != 0 || c->gpr[5]._u32[0] != entry || c->gpr[6]._u32[0] != 0x2C000000u ||
        c->gpr[1]._u32[0] != 0x3F990u || c->gpr[1]._u32[1] != 0x1980u || c->gpr[1]._u32[2] != 0 ||
        c->gpr[1]._u32[3] != 0)
        ERR(g_err_regs);
    /* a subroutine's frame and the SPU ABI stack check: r1 drops by the frame
     * in every word, and word 1 (bytes still available) must stay >= 0 */
    {
        const uint32_t frame = 0xC0;
        u128 sp = c->gpr[1];
        for (int i = 0; i < 4; i++) sp._u32[i] -= frame;
        if ((int32_t)sp._u32[1] < 0) ERR(g_err_stack);
    }
    const uint8_t* x = c->ls + ctx_ls;
    const uint32_t ud = be32r(x + 0x20);
    if (be32r(x + 0x24) != ud || be32r(x + 0x28) != ud || be32r(x + 0x2C) != ud || (ud & 15)) ERR(g_err_ud);
    const uint8_t* u = c->ls + ud;
    const uint32_t k = be32r(u), out = be32r(u + 4), sum = be32r(u + 8);
    if (k >= s_jobs || be32r(u + 0x1C) != 0x55D47A00u || be32r(u + 0x0C) != module) { ERR(g_err_ud); goto done; }
    __atomic_add_fetch(&g_runs[k], 1, __ATOMIC_ACQ_REL);
    const uint32_t lane = be32r(x + 0x00);
    if (lane >= NSPUS || be32r(x + 0x04) != 8 || be32r(x + 0x08) != 0 || be32r(x + 0x0C) != SPURS_EA ||
        be32r(x + 0x10) != (s_job[k].wid << 16 | s_job[k].index) || be32r(x + 0x14) != be32r(u + 0x18) ||
        be32r(x + 0x18) != ((s_job[k].list + 0x0A000000u) | s_job[k].array) ||
        be32r(x + 0x1C) != (s_job[k].wid << 16 | 0xFFFFu))
        ERR(g_err_ctx);
    /* the fixed low local store (spec 13.4) */
    {
        const uint8_t* ls = c->ls;
        const uint32_t v = be32r(ls), target = ((v >> 7) & 0xFFFFu) << 2;
        int bad = (v >> 23) != 0x060u || be32r(ls + 4) != v || be32r(ls + 8) != v || be32r(ls + 12) != v ||
                  target < 0x900u || target >= 0x9B0u || (target & 15) ||
                  target == be32r(x + 0x34) || target == be32r(x + 0x44) || target == be32r(x + 0x48);
        for (uint32_t o = 0x10; o < 0x80; o++) bad |= ls[o] != 0;
        bad |= be32r(ls + 0x1C0) != 0 || be32r(ls + 0x1C4) != SPURS_EA || be32r(ls + 0x1C8) != lane ||
               be32r(ls + 0x1CC) != 8 || be32r(ls + 0x1D0) != 0 || be32r(ls + 0x1D4) != 0xA00u ||
               be32r(ls + 0x1D8) != s_job[k].wid || be32r(ls + 0x1DC) != s_job[k].wid ||
               be32r(ls + 0x1E0) != 0x9C0u || be32r(ls + 0x1E4) != 0x9D0u;
        for (uint32_t o = 0x100; o < 0x1C0; o++) bad |= ls[o] != 0;
        /* interrupts enabled, no event enabled (spec 5.1) */
        if (spu_rdch(c, SPU_RdMachStat)._u32[0] != 1u || spu_rdch(c, SPU_RdEventMask)._u32[0] != 0) ERR(g_err_irq);
        bad |= ctx_ls != 0x1330u || ud < 0xC00u || ud >= 0xD00u;
        if (bad) ERR(g_err_lowls);
        if (g_irq_test && __atomic_exchange_n(&g_irq_test, 0, __ATOMIC_ACQ_REL)) {
            c->gpr[0] = spu_link(JOB_SITE + 4); c->pc = target; c->host_depth++;
            spu_indirect_branch(c); spu_drain_call(c, JOB_SITE + 4);
            c->host_depth--;
        }
    }
    c->int_enable = 0;   /* the job's own toggle (`bid`): services run with interrupts off */
    /* the job's scratch: its reservation buffer at 0x80 (a GETLLAR of its list
     * line, as the jobs do) and the rest of 0x80-0x17F */
    mfc(c, MFC_GETLLAR_CMD, 0x80, s_job[k].list, 128, 0);
    (void)spu_rdch(c, MFC_RdAtomicStat);
    memset(c->ls + 0x80, 0xA5, 0x100);
    /* the module the record named, at its load base */
    if (memcmp(c->ls + 0x4000, vm_base + (module ? MOD_B_EA : MOD_A_EA), MOD_BYTES) != 0) ERR(g_err_module);
    /* inputs: get buffer 1 (with a tag), the gathered pieces at the list rule's places */
    uint32_t d[4];
    call_service(c, ctx_ls, 0x34, 1, 0, 1, d);
    const uint32_t in_ls = (d[1] >> 16) * 4, in_len = (d[1] & 0xFFFF) * 4;
    if ((d[0] >> 24) != 1 || ((d[0] >> 16) & 0xFF) != 0 || in_len != 0x800 || (in_ls != 0x8000 && in_ls != 0x8800) ||
        d[2] != GLIST_EA + 0x40u * k || d[3] != 0x20)
        ERR(g_err_desc);
    if ((d[0] & 0xFF) != 0x1F) ERR(g_err_tag);
    struct piece pc[4];
    layout(IN_EA + 0x200u * k, pc);
    uint32_t got = 0;
    for (unsigned i = 0; i < 4; i++)
        for (uint32_t b = 0; b < pc[i].size; b++) {
            const uint8_t v = c->ls[in_ls + pc[i].off + b];
            if (v != vm_base[pc[i].ea + b]) { ERR(g_err_gather); i = 4; break; }
            got += v;
        }
    if (got != sum) ERR(g_err_gather);
    /* work buffer, no tag */
    uint32_t w[4];
    call_service(c, ctx_ls, 0x34, 3, 0, 0, w);
    const uint32_t work = (w[1] >> 16) * 4;
    if ((w[0] & 0xFF) != 0 || work != 0xC000 || (w[1] & 0xFFFF) * 4 != 0x400) ERR(g_err_desc);
    { const struct timespec d = { 0, 500000 }; nanosleep(&d, NULL); }   /* a job takes a while: lanes overlap */
    spu_ls_write32(c, work + 0, k); spu_ls_write32(c, work + 4, got);
    spu_ls_write32(c, work + 8, lane + 1); spu_ls_write32(c, work + 12, OUT_MAGIC);
    mfc(c, MFC_PUT_CMD, work, out, 16, 0x1F);
    spu_wrch(c, MFC_WrTagMask, spu_make_preferred_u32(1u << 0x1F));
    spu_wrch(c, MFC_WrTagUpdate, spu_make_preferred_u32(2));
    (void)spu_rdch(c, MFC_RdTagStat);
    if (call_service(c, ctx_ls, 0x48, 1, 0, 0, NULL) != 0xFFFFFFFFu) ERR(g_err_desc);
    uint32_t f[4];
    call_service(c, ctx_ls, 0x44, 0x1234, 0x5678, 0x9ABC, f);   /* garbage arguments */
    if (!(f[0] == 0 && f[1] == 0xFFFFFFFFu) && (f[0] & 0xFFFF) != 1) ERR(g_err_finish);
    if ((f[0] & 0xFFFF) == 1) ERR(g_finish_jobs);     /* finish claimed the next job */
    if (spu_rdch(c, SPU_RdMachStat)._u32[0] != 0) ERR(g_err_irq);   /* a service changed the bit */
    /* returns with interrupts off and an event enabled (no event is pending) */
    spu_wrch(c, SPU_WrEventMask, spu_make_preferred_u32(0x2u));
    /* after its last service call the job leaves junk in the low local store the
     * manager must plant before every job (spec 13.4: "before entering any job";
     * the runtime's own kernel-context write happens only at the run's start) */
    memset(c->ls + 0x10, 0xC3, 0x70);
    memset(c->ls + 0x180, 0x3C, 0x68);
done:
    c->gpr[0] = link;
    c->gpr[1] = spu_make_preferred_u32(0x3F990u);
    SPU_RET(c);
}
static void job_a(spu_context* c) { job_common(c, 0, 0x4000 + ENTRY_A); }
static void job_b(spu_context* c) { job_common(c, 1, 0x4000 + ENTRY_B); }

/* The lifted stand-in for the policy image: does nothing but exit. */
static void lifted_pm(spu_context* c)
{
    __atomic_add_fetch(&g_lifted_runs, 1, __ATOMIC_ACQ_REL);
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
    g_spu_trampoline_fn = spu_indirect_branch;
}

/* ---- the PPU's completion predicate, checked while the lanes run --------------------- */
static volatile int g_stop_monitor;
static int s_kick;
static void* monitor(void* arg)
{
    (void)arg;
    uint8_t a[128], b[128];
    while (!__atomic_load_n(&g_stop_monitor, __ATOMIC_ACQUIRE)) {
        do { memcpy(a, vm_base + LIST_EA, 128); memcpy(b, vm_base + LIST_EA, 128); } while (memcmp(a, b, 128));
        for (unsigned arr = 0; arr < 2; arr++) {
            uint32_t frontier = (uint32_t)(a[0x42 + 16 * arr] << 8 | a[0x43 + 16 * arr]);
            for (unsigned s = 0; s < 6; s++) {
                const uint32_t m = (uint32_t)(a[0x44 + 16 * arr + 2 * s] << 8 | a[0x45 + 16 * arr + 2 * s]);
                if (m != 0xFFFF && m < frontier) frontier = m;
            }
            for (unsigned k = 0; k < s_jobs; k++)
                if (s_job[k].wid == WID && s_job[k].array == arr && s_job[k].index < frontier &&
                    vm_read32(OUT_EA + 0x10u * k + 12) != OUT_MAGIC && be32r(vm_base + OUT_EA + 0x10u * k + 12) != OUT_MAGIC)
                    ERR(g_violations);
        }
        /* the PPU keeps the first workload kicked while it has work (never the
         * second: only the gate counter's wake may start that one) */
        if (s_kick && be16r(LIST_EA + 0x52) != be16r(LIST_EA + 0x50)) vm_write8(SPURS_EA + SPURS_WKL_READY1 + WID, NSPUS);
        const struct timespec d = { 0, 50000 }; nanosleep(&d, NULL);
    }
    return NULL;
}

/* A PPU-side writer that keeps changing a word of the line the manager never
 * reads (array 2's attach flag): store-conditionals that straddle it fail. */
static void* breaker(void* arg)
{
    (void)arg;
    while (!__atomic_load_n(&g_stop_monitor, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_xor(&vm_base[LIST_EA + 0x1B], 1, __ATOMIC_SEQ_CST);
        if ((__atomic_add_fetch(&g_breaks, 1, __ATOMIC_RELAXED) & 63) == 0) sched_yield();
    }
    return NULL;
}

static struct spurs_native_pm s_impl;

int main(int argc, char** argv)
{
    vm_base = (uint8_t*)calloc(1, GUEST); ppu_vm_size = GUEST;
    { extern uint8_t g_vm_page_bitmap[65536 / 8]; memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap); }
    const int native = argc > 1 && (!strcmp(argv[1], "native") || !strcmp(argv[1], "native-irq"));
    g_irq_test = argc > 1 && !strcmp(argv[1], "native-irq");
    for (uint32_t i = 0; i < PM_SIZE; i++) vm_base[PM_EA + i] = (uint8_t)(i * 29u + 3u);
    const uint64_t fp = spu_workload_fingerprint(vm_base + PM_EA, PM_SIZE);
    spu_workload_register_img(fp, lifted_pm, IMAGE, "fixture");
    spu_begin_image(IMAGE);
    spu_register_function(0x4000 + ENTRY_A, job_a);
    spu_register_function(0x4000 + ENTRY_B, job_b);
    if (native) {
        s_impl = *spurs_wwsjm_native();
        s_impl.name = "wwsjm-fixture";
        s_impl.fingerprint = fp;
        spurs_native_pm_sha256(vm_base + PM_EA, PM_SIZE, s_impl.sha256);
        assert(spurs_native_pm_register(&s_impl) == 0);
    }
    build_list();
    build_list2();
    assert(spurs_initialize_common(SPURS_EA, NSPUS, "fixture") == CELL_OK);
    s_workloads[WID].in_use = 1; s_workloads[WID].is_taskset = 0;
    s_workloads[WID].pm = (const void*)(uintptr_t)PM_EA; s_workloads[WID].sizePm = PM_SIZE;
    s_workloads[WID].data = LIST_EA; s_workloads[WID].spurs_ea = SPURS_EA;
    vm_write64(SPURS_EA + SPURS_WKL_INFO1 + WID * SPURS_WKL_INFO_SZ + 8, LIST_EA);
    vm_base[SPURS_EA + SPURS_WKL_STATE1 + WID] = 2;
    vm_base[SPURS_EA + SPURS_WKL_MAXCONT + WID] = NSPUS;
    for (unsigned i = 0; i < 8; i++) vm_base[SPURS_EA + SPURS_WKL_INFO1 + WID * SPURS_WKL_INFO_SZ + 0x18 + i] = 1;
    s_workloads[WID2] = s_workloads[WID];
    s_workloads[WID2].data = LIST2_EA;
    vm_write64(SPURS_EA + SPURS_WKL_INFO1 + WID2 * SPURS_WKL_INFO_SZ + 8, LIST2_EA);
    vm_base[SPURS_EA + SPURS_WKL_STATE1 + WID2] = 2;
    vm_base[SPURS_EA + SPURS_WKL_MAXCONT + WID2] = NSPUS;
    for (unsigned i = 0; i < 8; i++) vm_base[SPURS_EA + SPURS_WKL_INFO1 + WID2 * SPURS_WKL_INFO_SZ + 0x18 + i] = 1;
    vm_write32(SPURS_EA + SPURS_WKL_ENABLED, (0x80000000u >> WID) | (0x80000000u >> WID2));
    pthread_t mon, brk;
    pthread_create(&mon, NULL, monitor, NULL);
    const int breaking = native && !getenv("WWSJM_FIXTURE_NO_BREAKER");
    s_kick = native;
    if (breaking) pthread_create(&brk, NULL, breaker, NULL);
    vm_write8(SPURS_EA + SPURS_WKL_READY1 + WID, NSPUS);          /* the PPU kicks the workload */
    const uint64_t t0 = now_ns();
    int done = 0;
    while (now_ns() - t0 < 10000000000ull) {
        unsigned n = 0;
        for (unsigned k = 0; k < s_jobs; k++) n += be32r(vm_base + OUT_EA + 0x10u * k + 12) == OUT_MAGIC;
        const int lanes_free = be16r(LIST_EA + 0x44) == 0xFFFF && be16r(LIST_EA + 0x54) == 0xFFFF;
        if (n == s_jobs && be16r(LIST_EA + 0x52) == be16r(LIST_EA + 0x50) && lanes_free &&
            be16r(LIST2_EA + 0x42) == 1 && be16r(LIST2_EA + 0x44) == 0xFFFF &&
            vm_read8(SPURS_EA + SPURS_WKL_CURCONT + WID) == 0 && vm_read8(SPURS_EA + SPURS_WKL_CURCONT + WID2) == 0) {
            done = 1; break; }
        if (!native && now_ns() - t0 > 500000000ull) break;
        const struct timespec d = { 0, 200000 }; nanosleep(&d, NULL);
    }
    const struct timespec settle = { 0, 30000000 }; nanosleep(&settle, NULL);
    __atomic_store_n(&g_stop_monitor, 1, __ATOMIC_RELEASE);
    pthread_join(mon, NULL);
    if (breaking) pthread_join(brk, NULL);
    unsigned ran = 0, dup = 0, missing = 0, bad_out = 0;
    for (unsigned k = 0; k < s_jobs; k++) {
        ran += g_runs[k] != 0; dup += g_runs[k] > 1; missing += g_runs[k] == 0;
        const uint8_t* o = vm_base + OUT_EA + 0x10u * k;
        if (g_runs[k] && (be32r(o) != k || be32r(o + 4) != s_job[k].sum || be32r(o + 8) == 0 || be32r(o + 8) > NSPUS))
            bad_out++;
    }
    unsigned lanes_busy = 0;
    for (unsigned a = 0; a < 2; a++) for (unsigned s = 0; s < 6; s++) lanes_busy += be16r(LIST_EA + 0x44 + 16 * a + 2 * s) != 0xFFFF;
    printf("result done=%d jobs=%u ran=%u dup=%u missing=%u bad_out=%u regs=%u ctx=%u ud=%u module=%u gather=%u desc=%u "
           "tag=%u finish=%u violations=%u lifted=%u current=%u n0=%u s0=%u n1=%u s1=%u lanes_busy=%u gate=%u "
           "switches=%04X consults=%lu finish_jobs=%u breaks=%u lowls=%u irq=%u stack=%u\n",
           done, s_jobs, ran, dup, missing, bad_out, g_err_regs, g_err_ctx, g_err_ud, g_err_module, g_err_gather,
           g_err_desc, g_err_tag, g_err_finish, g_violations, g_lifted_runs, be16r(LIST_EA + 0x0A),
           be16r(LIST_EA + 0x42), be16r(LIST_EA + 0x40), be16r(LIST_EA + 0x52), be16r(LIST_EA + 0x50), lanes_busy,
           be16r(GATE_EA), be16r(LIST_EA + 0x0E), spurs_native_pm_consults(), g_finish_jobs, g_breaks, g_err_lowls, g_err_irq, g_err_stack);
    fflush(stdout);
    if (native) spurs_wwsjm_print_stats();
    if (native && getenv("PS3RECOMP_SPURS_NATIVE_PM_SELF_SHADOW")) spurs_wwsjm_shadow_print();
    _exit(0);
}
