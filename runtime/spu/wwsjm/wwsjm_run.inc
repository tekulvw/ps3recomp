/* wwsjm_run.inc -- the native WWS job manager: one policy-module dispatch as
 * host code (docs/SPURS_NATIVE_WWSJM.md). Included once, by
 * spurs_native_pm.inc; registered only when PS3RECOMP_SPURS_NATIVE_PM selects
 * native implementations, so with the switch unset nothing here runs.
 *
 * Written from the job manager's behavioural specification, not from the
 * original program. The pure decisions (line transactions, plans, placement,
 * descriptors, residency) are wwsjm_core.c; this file performs them on a
 * slot's resident context: line atomics and staging GETs through the
 * context's MFC channels, guest reads of headers, records and lists, the job
 * API context in a local-store area of its own, job entry through the seam,
 * and the three job services through the native-service window. */
#define WWSJM_API static inline
#include "wwsjm/wwsjm_core.c"

extern void spu_wrch(spu_context*, uint32_t, u128);
extern u128 spu_rdch(spu_context*, uint32_t);

/* The policy image the specification describes (identity only). */
#define WJ_FINGERPRINT UINT64_C(0xAD16923605435AD0)
#define WJ_SHA256 "6a3af90518af72ed790b36b2081ab2040237d378bf3644d4461356b44bcfc58a"

/* Local store below the job window (spec 13.4). Job-facing, at the addresses
 * the jobs were observed to read (spec 13.2, 5.2): the interrupt-vector
 * quadword at 0 and zeros to 0x7F, the SPURS kernel context at 0x100-0x1E7,
 * the command copy (and so the userdata) at 0xC00, the job API context at
 * 0x1330. 0x80-0x17F belongs to the running job (its reservation buffers).
 * The manager's own scratch is anywhere else below 0x4000. */
enum {
    WJ_JOB_SCRATCH_LS = 0x80,  /* [0x80, 0x180): the job's while it runs */
    WJ_READ_LS = 0x200,       /* where header and gate reads are reported (the
                                 manager reads guest memory directly; spec 17.1
                                 keeps it out of 0x000-0x1FF) */
    WJ_JOB_SCRATCH_END = 0x180,
    WJ_KCTX_LS = 0x100,       /* SPURS kernel context (spec 3.1)    */
    WJ_KCTX_END = 0x1E8,
    WJ_PORTION_LS = 0xC00,    /* the record's command portion       */
    WJ_LINE_LS = 0x1000,      /* job list line (atomics)            */
    WJ_INST_LS = 0x1080,      /* SPURS instance line (ready counts) */
    WJ_DEP_LS = 0x1100,       /* dependency counter line            */
    WJ_CTX_LS = 0x1330,       /* job API context (0x80 bytes)       */
    WJ_STACK = 0x3F990,       /* job stack top (spec 5.1)           */
    WJ_STACK_BYTES = 0x1980,  /* r1 word 1: stack bytes available (spec 5.1, 15) */
    WJ_STACK_BOTTOM = WJ_STACK - WJ_STACK_BYTES,   /* 0x3E010: [BOTTOM, WJ_STACK) is the job's */
    WJ_LS_LOW = 0x4000,       /* job buffers live in [LOW, HIGH)    */
    WJ_LS_HIGH = 0x3E000,     /* up to the job stack: a set may end at 0x3E000 (observed) */
    WJ_STAGE_TAG = 12,        /* the manager's own staging tag: outside the jobs' 16..31 */
    WJ_ENTRY_R6 = 0x2C000000, /* reproduced as observed (spec U14)  */
    WJ_GATHER_LIST_MAX = 1024 /* bytes (64 quadwords; observed <= 12) */
};
/* Window slots for the context's service words +0x30..+0x48 (slot i = word i).
 * Only get-buffer, finish and release exist; the others are left empty, and a
 * job that branches to one stops the process (the seam's loud refusal). */
enum { WJ_SVC_GET_BUFFER = 1, WJ_SVC_FINISH = 5, WJ_SVC_RELEASE = 6 };
/* The slot the interrupt-vector quadword at LS 0 branches to (spec 13.4): never
 * entered, since the native run raises no SPU events; it halts loudly. */
enum { WJ_SVC_IRQ_LANDING = 7 };

/* No manager area overlaps a job-facing range or the job's scratch. */
#define WJ_DISJOINT(a, an, b, bn) ((a) + (an) <= (b) || (b) + (bn) <= (a))
_Static_assert(WJ_DISJOINT(WJ_PORTION_LS, WWSJM_MAX_PORTION, 0, WJ_KCTX_END) &&
               WJ_DISJOINT(WJ_PORTION_LS, WWSJM_MAX_PORTION, WJ_CTX_LS, 0x80) &&
               WJ_DISJOINT(WJ_PORTION_LS, WWSJM_MAX_PORTION, WJ_LINE_LS, 0x180), "command copy overlaps");
_Static_assert(WJ_DISJOINT(WJ_LINE_LS, 0x180, 0, WJ_KCTX_END) && WJ_DISJOINT(WJ_LINE_LS, 0x180, WJ_CTX_LS, 0x80) &&
               WJ_DISJOINT(WJ_CTX_LS, 0x80, 0, WJ_KCTX_END), "manager area overlaps a job-facing range");
_Static_assert(SPURS_NATIVE_PM_SLOT_LS(WJ_SVC_IRQ_LANDING) < SPURS_NATIVE_PM_JOB_RETURN_LS, "landing slot");
/* The job stack [0x3E010, 0x3F990) holds nothing of the manager's or of the job window. */
_Static_assert(WJ_STACK_BOTTOM == 0x3E010 && WJ_LS_HIGH <= WJ_STACK_BOTTOM &&
               WJ_LINE_LS + 0x180 <= WJ_STACK_BOTTOM && WJ_CTX_LS + 0x80 <= WJ_STACK_BOTTOM &&
               WJ_PORTION_LS + WWSJM_MAX_PORTION <= WJ_STACK_BOTTOM, "job stack overlaps");

enum { WJ_TAGS = WWSJM_JOB_TAG_COUNT };

struct wj_job {
    int valid;
    unsigned array;
    uint16_t index;
    wwsjm_header h;
    wwsjm_plan plan;          /* the record's plan, made and checked before the claim (wj_preflight) */
};

struct wj_run {
    spu_context* ctx;
    uint32_t list_ea, spurs_ea, wid, lane, busy, k_inst;
    int check;
    wwsjm_residency res;
    struct wj_job cur, next;
    wwsjm_plan plan;
    unsigned tags, finishes;
    struct { uint8_t set, buf, tag; } tag_of[WJ_TAGS];   /* (set, buffer) -> DMA tag, this job */
    unsigned tag_n;
    uint32_t released;
    int stop;                 /* the kernel asked for another workload */
    /* A refusal met before a claim withdraws the implementation
     * (spurs_native_pm_withdraw, wj_refuse) when the lifted module can take
     * over and the mode is not `check`; the run then claims nothing more and
     * ends once it holds no job. */
    int fallback_ok, withdraw;
};
static SPU_THREAD_LOCAL struct wj_run* t_wj;

/* Instances the implementation took (accept), with their SPU counts. */
static struct { uint32_t spurs_ea, n_spus; } s_wj_inst[4];
static SRWLOCK s_wj_lock = SRWLOCK_INIT;

static struct {
    unsigned long long runs, idle_runs, jobs, claims, nonjob_claims, retires, decrements, wakes,
        ready_writes, putllc_retries, polls, loads, load_skips, gathers, nops, set0_moved, index_watch,
        verifies, verify_work, flag_polls, flag_entries, gather_overlaps, withdrawals;
} s_wj_stats;
#define WJ_COUNT(f, n) __atomic_add_fetch(&s_wj_stats.f, (unsigned long long)(n), __ATOMIC_RELAXED)

void spurs_wwsjm_print_stats(void);
static void wj_print_stats(void)
{
#define L(f) __atomic_load_n(&s_wj_stats.f, __ATOMIC_RELAXED)
    fprintf(stderr, "[wwsjm] runs=%llu idle=%llu jobs=%llu claims=%llu nonjob=%llu retires=%llu decrements=%llu "
            "wakes=%llu ready_writes=%llu putllc_retries=%llu polls=%llu loads=%llu resident_skips=%llu "
            "gathers=%llu nops=%llu set0_moved=%llu index_watch=%llu verifies=%llu verify_work=%llu flag_polls=%llu flag_entries=%llu gather_overlaps=%llu withdrawals=%llu\n",
            L(runs), L(idle_runs), L(jobs), L(claims), L(nonjob_claims), L(retires), L(decrements), L(wakes),
            L(ready_writes), L(putllc_retries), L(polls), L(loads), L(load_skips), L(gathers), L(nops),
            L(set0_moved), L(index_watch), L(verifies), L(verify_work), L(flag_polls), L(flag_entries), L(gather_overlaps),
            L(withdrawals));
#undef L
    fflush(stderr);
}

static void wj_fatal(const struct wj_run* r, const char* what, uint32_t a, uint32_t b)
{
    fprintf(stderr, "[wwsjm] FATAL %s (0x%X 0x%X) wid=%u lane=%u list=0x%08X job=%s%u:%u\n", what, a, b,
            r ? r->wid : 0, r ? r->lane : 0, r ? r->list_ea : 0, r && r->cur.valid ? "" : "none ",
            r ? r->cur.array : 0, r ? r->cur.index : 0);
    if (r && r->list_ea && vm_base) {
        fprintf(stderr, "[wwsjm] line");
        for (unsigned i = 0; i < 128; i += 4)
            fprintf(stderr, " %08X", wwsjm_be32(vm_base + r->list_ea + i));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    _Exit(SPURS_NATIVE_PM_FATAL_EXIT);
}

/* Whether a refusal's report (the record or gather list, the line) should
 * print: always when it stops the process; when it withdraws, only until the
 * implementation is withdrawn, so concurrent lanes add at most a few. */
static int wj_report_refusal(const struct wj_run* r)
{
    return !r->fallback_ok || (!r->withdraw && !spurs_native_pm_withdrawn(t_npm_pm));
}

/* Guest state outside the contract, met before the entry is claimed: nothing
 * of it is done, and a job this run is already running finishes normally, so
 * once the run holds no job the lifted module can take the job list over.
 * Withdraws the implementation (one line, with the same report a stop prints)
 * unless the mode is `check` or no lifted module exists, which stop as before.
 * Returns 0, so callers can `return wj_refuse(...)` as "nothing claimable". */
static int wj_refuse(struct wj_run* r, const char* what, uint32_t a, uint32_t b)
{
    if (!r->fallback_ok) wj_fatal(r, what, a, b);
    if (!r->withdraw) {
        static int s_reported;
        r->withdraw = 1;
        /* the implementation this thread is running (spurs_native_pm.inc); the
         * first lane to refuse reports, once per process */
        if (!spurs_native_pm_withdrawn(t_npm_pm) && !__atomic_exchange_n(&s_reported, 1, __ATOMIC_ACQ_REL)) {
            char why[192];
            snprintf(why, sizeof why, "%s (0x%X 0x%X) wid=%u lane=%u list=0x%08X", what, a, b, r->wid, r->lane,
                     r->list_ea);
            if (r->list_ea && vm_base) {
                fprintf(stderr, "[wwsjm] refused before a claim: %s; line", why);
                for (unsigned i = 0; i < 128; i += 4) fprintf(stderr, " %08X", wwsjm_be32(vm_base + r->list_ea + i));
                fprintf(stderr, "\n");
            }
            spurs_native_pm_withdraw(t_npm_pm, why);
        }
    }
    return 0;
}

/* ---- guest access ------------------------------------------------------------ */

/* Guest reads go to guest memory directly; when a policy-run observer is armed
 * (the recorder, a shadow comparison) each is reported as a GET of `n` bytes
 * to `lsa` so diagnostics still see every access. */
static void wj_guest_read(const struct wj_run* r, uint32_t ea, uint32_t n, uint8_t* out, uint32_t lsa)
{
    uint64_t limit = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    if (!vm_base || (uint64_t)ea + n > limit)
        wj_fatal(r, "guest read out of range", ea, n);
    memcpy(out, vm_base + ea, n);
    if (r && spu_pm_extras_on() && r->ctx->pm_observer)
        r->ctx->pm_observer->transfer(r->ctx, SPURS_PM_XFER_GET, MFC_GET_CMD, lsa, ea, n, out);
}
/* An 8-byte header, read as one atomic load when it is 8-aligned (it always is:
 * header arrays are 8-byte entries), so a publisher's concurrent write is seen
 * whole or not at all. */
static void wj_guest_read8(const struct wj_run* r, uint32_t ea, uint8_t out[8], uint32_t lsa)
{
    uint64_t limit = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    if (!vm_base || (uint64_t)ea + 8 > limit) wj_fatal(r, "guest read out of range", ea, 8);
    if (ea & 7) { wj_guest_read(r, ea, 8, out, lsa); return; }
    const uint64_t v = __atomic_load_n((const uint64_t*)(const void*)(vm_base + ea), __ATOMIC_ACQUIRE);
    memcpy(out, &v, 8);
    if (r && spu_pm_extras_on() && r->ctx->pm_observer)
        r->ctx->pm_observer->transfer(r->ctx, SPURS_PM_XFER_GET, MFC_GET_CMD, lsa, ea, 8, out);
}


/* A guest read for a check made before a claim (wj_preflight): never reported
 * to an observer, since the run reads the same bytes again, reported, when it
 * stages the job. Returns 0 when the range is outside guest memory. */
static int wj_guest_peek(uint32_t ea, uint32_t n, uint8_t* out)
{
    uint64_t limit = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    if (!vm_base || (uint64_t)ea + n > limit) return 0;
    memcpy(out, vm_base + ea, n);
    return 1;
}

static void wj_mfc(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    spu_wrch(c, MFC_LSA, spu_make_preferred_u32(lsa));
    spu_wrch(c, MFC_EAH, spu_make_preferred_u32(0));
    spu_wrch(c, MFC_EAL, spu_make_preferred_u32(ea));
    spu_wrch(c, MFC_Size, spu_make_preferred_u32(size));
    spu_wrch(c, MFC_TagID, spu_make_preferred_u32(tag));
    spu_wrch(c, MFC_Cmd, spu_make_preferred_u32(cmd));
}

static void wj_getllar(struct wj_run* r, uint32_t lsa, uint32_t ea, uint8_t line[128])
{
    wj_mfc(r->ctx, MFC_GETLLAR_CMD, lsa, ea, 128, 0);
    (void)spu_rdch(r->ctx, MFC_RdAtomicStat);
    memcpy(line, r->ctx->ls + lsa, 128);
}

/* Returns 1 when the store-conditional succeeded. */
static int wj_putllc(struct wj_run* r, uint32_t lsa, uint32_t ea, const uint8_t line[128])
{
    memcpy(r->ctx->ls + lsa, line, 128);
    wj_mfc(r->ctx, MFC_PUTLLC_CMD, lsa, ea, 128, 0);
    if (spu_rdch(r->ctx, MFC_RdAtomicStat)._u32[0] == 0) return 1;
    WJ_COUNT(putllc_retries, 1);
    return 0;
}

static void wj_wait_tag(spu_context* c, uint32_t tag)
{
    spu_wrch(c, MFC_WrTagMask, spu_make_preferred_u32(1u << tag));
    spu_wrch(c, MFC_WrTagUpdate, spu_make_preferred_u32(2));   /* all */
    (void)spu_rdch(c, MFC_RdTagStat);
}

/* ---- check mode: a line transaction changes only what the contract allows ---- */

static void wj_check_line(const struct wj_run* r, const uint8_t* old, const uint8_t* now, unsigned array,
                          int switch_ok)
{
    for (unsigned i = 0; i < 128; i++) {
        if (old[i] == now[i]) continue;
        const int markers = i >= 0x40u + 16u * array && i < 0x50u + 16u * array;
        const int switching = switch_ok && (i == 0x0A || i == 0x0B || (i >= 0x0C && i <= 0x0F));
        if (!markers && !switching) wj_fatal(r, "check: line byte outside the contract changed", i, now[i]);
    }
    /* other lanes' markers are never written */
    for (unsigned s = 0; s < WWSJM_LANES; s++) {
        if (s == r->lane) continue;
        const unsigned o = 0x44u + 16u * array + 2u * s;
        if (old[o] != now[o] || old[o + 1] != now[o + 1]) wj_fatal(r, "check: another lane's marker changed", s, o);
    }
}

/* ---- decisions on the current line ---------------------------------------------- */

/* The entry at N of the current array of `line`, and whether it is claimable. */
static wwsjm_verdict wj_examine(struct wj_run* r, const uint8_t line[128], wwsjm_cursor* cur, wwsjm_header* h)
{
    wwsjm_line dl;
    wwsjm_line_decode(line, &dl);
    wwsjm_status st = wwsjm_cursor_of(&dl, cur);
    if (st != WWSJM_OK) return (wwsjm_verdict)wj_refuse(r, "current array malformed", dl.current, st);
    if (cur->index >= WWSJM_INDEX_WRAP_WATCH && __atomic_add_fetch(&s_wj_stats.index_watch, 1, __ATOMIC_RELAXED) <= 4)
        fprintf(stderr, "[wwsjm] index 0x%04X at or above 0x%04X on array %u (wid %u): the u16 wrap is unobserved\n",
                cur->index, WWSJM_INDEX_WRAP_WATCH, cur->array, r->wid);
    uint8_t raw[8];
    wj_guest_read8(r, cur->header_ea, raw, WJ_READ_LS);
    st = wwsjm_header_decode(raw, h);
    if (st != WWSJM_OK)
        return (wwsjm_verdict)wj_refuse(r, "entry header outside the contract", wwsjm_be32(raw), wwsjm_be32(raw + 4));
#ifdef WWSJM_TEST_HOOKS
    {   /* synthetic fixtures only: publish concurrently at this exact point */
        extern void wwsjm_test_header_read(uint32_t list_ea, uint16_t index, unsigned type);
        wwsjm_test_header_read(r->list_ea, cur->index, h->type);
    }
#endif
    uint16_t gate = 0;
    if (wwsjm_header_is_gate(h)) {
        uint8_t g[2];
        wj_guest_read(r, h->ea, 2, g, WJ_READ_LS);
        gate = wwsjm_be16(g);
    }
    return wwsjm_judge(h, gate);
}

static void wj_ready_store(struct wj_run* r, unsigned wid, uint8_t value)
{
    uint8_t old[128], now[128];
    for (;;) {
        wj_getllar(r, WJ_INST_LS, r->spurs_ea, old);
        if (wwsjm_ready_apply(old, wid, value, now) != WWSJM_OK) wj_fatal(r, "ready count of a workload >= 16", wid, value);
        if (wj_putllc(r, WJ_INST_LS, r->spurs_ea, now)) break;
    }
    WJ_COUNT(ready_writes, 1);
#ifdef WWSJM_TEST_HOOKS
    { extern void wwsjm_test_ready_store(uint32_t list_ea, unsigned wid, uint8_t value);
      wwsjm_test_ready_store(r->list_ea, wid, value); }
#endif
}

/* Spec 6.4 on the line as it is now: busy while counted entries remain and the
 * entry at N is claimable. */
static uint8_t wj_ready_rule(struct wj_run* r)
{
    uint8_t line[128];
    wj_getllar(r, WJ_LINE_LS, r->list_ea, line);
    wwsjm_cursor cur;
    wwsjm_header h;
    const wwsjm_verdict v = wj_examine(r, line, &cur, &h);
    wwsjm_line dl;
    wwsjm_line_decode(line, &dl);
    const uint8_t value = wwsjm_ready_value(&dl, v, (uint8_t)r->busy);
    wj_ready_store(r, r->wid, value);
    return value;
}

/* A select-workload poll; a status that reports this workload's flag is
 * followed by ready := 8, as at entry (spec 6.1, U19). Returns the answer. */
static uint32_t wj_poll(struct wj_run* r)
{
    spurs_kernel_select_workload(r->ctx);
    WJ_COUNT(polls, 1);
    const uint32_t answer = r->ctx->gpr[3]._u32[0], status = r->ctx->gpr[3]._u32[1];
    if (status == 4) { wj_ready_store(r, r->wid, WWSJM_READY_ON_FLAG); WJ_COUNT(flag_polls, 1); }
    return answer;
}

/* The verify of spec 6.5 / 16.2 R12: a fresh line read and the header at N read
 * after it. Returns 1 when the entry at N is claimable (the claim loop goes
 * on). Records what it saw for the exit check (R14). */
static int wj_verify(struct wj_run* r)
{
    uint8_t line[128];
    wj_getllar(r, WJ_LINE_LS, r->list_ea, line);
    wwsjm_cursor cur;
    wwsjm_header h;
    const wwsjm_verdict v = wj_examine(r, line, &cur, &h);
    WJ_COUNT(verifies, 1);
    if (v == WWSJM_TAKE) { WJ_COUNT(verify_work, 1); return 1; }
    if (r->check) {
        /* R14: S may exceed N at exit only while the entry at N is not yet
         * visible (type 0) or is a gate still closed */
        wwsjm_line dl;
        wwsjm_line_decode(line, &dl);
        const unsigned a = dl.current;
        if (a < WWSJM_ARRAYS && dl.m[a].next != dl.m[a].submitted &&
            h.type != WWSJM_ENTRY_EMPTY && !wwsjm_header_is_gate(&h))
            wj_fatal(r, "check: exit with S beyond N and a visible entry at N (R14)", h.type, cur.index);
    }
    return 0;
}

/* The gather of load `l` with its (size, EA) list `list` (spec 17.1, 18.1): the
 * list is copied to the last 16*L bytes of the buffer, where the job reads it;
 * the elements land from the buffer's start under MFC list rules. An element
 * may run into the copy over entries already consumed: element k must end at
 * or before the copy's start + 8*(k+1), and inside the buffer. Fills `pc`
 * (`cap` entries) and `*n`; `*overlap` says an element ran into the copy. */
static wwsjm_status wj_gather_plan(const wwsjm_load* l, const uint8_t* list, wwsjm_piece* pc, unsigned cap,
                                   unsigned* n, unsigned* overlap)
{
    const uint32_t list_ls = l->ls + l->limit - l->bytes;
    *overlap = 0;
    wwsjm_status st = wwsjm_gather_layout(list, l->bytes / 8, l->ls, l->limit, pc, cap, n);
    if (st != WWSJM_OK) return st;
    /* list entry k behind each piece (zero-size entries have none) */
    unsigned piece = 0;
    for (unsigned k = 0; k < l->bytes / 8 && piece < *n; k++) {
        if (!wwsjm_be32(list + 8u * k)) continue;
        const uint32_t end = pc[piece].ls + pc[piece].bytes;
        if (end > list_ls) {
            if (end > list_ls + 8u * (k + 1)) return WWSJM_E_GATHER;
            *overlap = 1;
        }
        piece++;
    }
    return WWSJM_OK;
}

/* Everything about a job entry that can refuse it, checked before its claim so
 * that a refusal leaves nothing held (wj_refuse): the record's commands, the
 * module set, where the loads land, every gather list and the module entry.
 * Reads guest memory without reporting it (the run reads the same bytes again,
 * reported, when it stages the job). The plan is kept for the job. Returns 1
 * when the job is within the contract. */
static int wj_preflight(struct wj_run* r, const wwsjm_header* h, wwsjm_plan* plan)
{
    uint8_t rec[WWSJM_MAX_PORTION];
    if (h->size > sizeof rec || !wj_guest_peek(h->ea, h->size, rec))
        return wj_refuse(r, "job record outside guest memory", h->ea, h->size);
    unsigned bad = 0;
    const wwsjm_status st = wwsjm_plan_commands(rec, h->size, WJ_LS_LOW, WJ_LS_HIGH, plan, &bad);
    if (st != WWSJM_OK) {
        if (wj_report_refusal(r)) {
            fprintf(stderr, "[wwsjm] refused record at 0x%08X (%u bytes, command %u):", h->ea, h->size, bad);
            for (uint32_t o = 0; o + 4 <= h->size; o += 4) fprintf(stderr, "%s%08X", (o & 7) ? " " : "  ", wwsjm_be32(rec + o));
            fprintf(stderr, "\n");
        }
        return wj_refuse(r, wwsjm_status_name(st), bad, wwsjm_be32(rec + 8 * (bad < 32 ? bad : 0)));
    }
    if (!plan->set[0].reserved || !plan->set[0].pages) return wj_refuse(r, "record without a module set", h->ea, 0);
    wwsjm_plan_place(plan, NULL, 0);      /* nothing else holds LS: no job runs beside this one */
    uint32_t set0_ls = 0, set0_bytes = 0, module_ea = 0;
    wwsjm_buffer_span(plan, 0, plan->set[0].first_buffer, &set0_ls, &set0_bytes);
    for (unsigned i = 0; i < plan->load_count; i++) {
        const wwsjm_load* l = &plan->load[i];
        if (l->ls < WJ_LS_LOW || l->ls >= WJ_LS_HIGH) return wj_refuse(r, "staged load outside the job window", l->ls, l->bytes);
        if (l->fill == WWSJM_FILL_COPY) {
            if (plan->use[l->use].set == 0 && l->ls == set0_ls) module_ea = l->ea;
            continue;
        }
        uint8_t list[WJ_GATHER_LIST_MAX];
        if (l->bytes > sizeof list) return wj_refuse(r, "gather list longer than supported", l->ea, l->bytes);
        if (l->bytes > l->limit || (l->bytes & 15)) return wj_refuse(r, "gather list does not fit its buffer", l->bytes, l->limit);
        if (!wj_guest_peek(l->ea, l->bytes, list)) return wj_refuse(r, "gather list outside guest memory", l->ea, l->bytes);
        wwsjm_piece pc[sizeof list / 8];
        unsigned n = 0, overlap = 0;
        const wwsjm_status gs = wj_gather_plan(l, list, pc, sizeof pc / sizeof pc[0], &n, &overlap);
        if (gs != WWSJM_OK && wj_report_refusal(r)) {
            fprintf(stderr, "[wwsjm] refused gather list at 0x%08X (%u bytes) into LS 0x%05X, room 0x%X, %s:",
                    l->ea, l->bytes, l->ls, l->limit, wwsjm_status_name(gs));
            for (uint32_t o = 0; o + 8 <= l->bytes; o += 8) fprintf(stderr, " (%X, %08X)", wwsjm_be32(list + o), wwsjm_be32(list + o + 4));
            fprintf(stderr, "\n");
        }
        if (gs != WWSJM_OK) return wj_refuse(r, "gather list outside the contract", l->ea, gs);
    }
    /* the module's entry, from the header of the image its set-0 copy loads */
    if (module_ea) {
        uint8_t off[4];
        if (!wj_guest_peek(module_ea + 0x10, 4, off)) return wj_refuse(r, "module header outside guest memory", module_ea, 0);
        const uint32_t entry = set0_ls + wwsjm_be32(off);
        if (entry < set0_ls || entry >= set0_ls + set0_bytes || (entry & 3))
            return wj_refuse(r, "module entry outside its set", entry, set0_ls);
    }
    return 1;
}

/* Claim entries at N until a job is claimed (returns 1, *job set) or the entry
 * at N is not claimable (returns 0). Non-job entries are claimed and passed.
 * Every decision is made on the line read just before it and the header read
 * after that line read (R10); a failed claim re-reads both (R11). */
static int wj_claim(struct wj_run* r, struct wj_job* job, int in_finish)
{
    uint8_t old[128], now[128];
    for (;;) {
        /* withdrawn (here or by another lane's refusal): claim nothing more */
        if (r->withdraw) return 0;
        if (r->fallback_ok && spurs_native_pm_withdrawn(t_npm_pm)) { r->withdraw = 1; return 0; }
        wj_getllar(r, WJ_LINE_LS, r->list_ea, old);
        wwsjm_cursor cur;
        wwsjm_header h;
        if (wj_examine(r, old, &cur, &h) != WWSJM_TAKE) return 0;
        if (h.type == WWSJM_ENTRY_JOB && !wj_preflight(r, &h, &job->plan)) return 0;
        if (wwsjm_claim(old, r->lane, &h, now) != WWSJM_OK) return wj_refuse(r, "claim refused", h.type, cur.index);
        if (r->check) wj_check_line(r, old, now, cur.array, h.type == WWSJM_ENTRY_SWITCH);
        if (!wj_putllc(r, WJ_LINE_LS, r->list_ea, now)) continue;   /* lost: re-read */
        WJ_COUNT(claims, 1);
        if (h.type == WWSJM_ENTRY_JOB) {
            job->valid = 1;
            job->array = cur.array;
            job->index = cur.index;
            job->h = h;
            return 1;
        }
        WJ_COUNT(nonjob_claims, 1);
        if (!in_finish) {              /* poll between entries, never inside a job */
            if (wj_poll(r) != r->wid) { r->stop = 1; return 0; }
        }
    }
}

/* ---- staging and entering a job ---------------------------------------------------- */

static void wj_stage(struct wj_run* r)
{
    spu_context* c = r->ctx;
    int issued = 0;
    for (unsigned i = 0; i < r->plan.load_count; i++) {
        const wwsjm_load* l = &r->plan.load[i];
        if (l->ls < WJ_LS_LOW || l->ls >= WJ_LS_HIGH) wj_fatal(r, "staged load outside the job window", l->ls, l->bytes);
        if (l->fill == WWSJM_FILL_COPY) {
            if (l->skip) { WJ_COUNT(load_skips, 1); continue; }
            if (r->check && (l->bytes > WWSJM_CHUNK_BYTES || l->bytes % 16 || l->ls % 16 || l->ea % 16))
                wj_fatal(r, "check: staged GET shape", l->ls, l->bytes);
            wj_mfc(c, MFC_GET_CMD, l->ls, l->ea, l->bytes, WJ_STAGE_TAG);
            issued = 1;
            WJ_COUNT(loads, 1);
            continue;
        }
        /* gather (wj_gather_plan) */
        uint8_t list[WJ_GATHER_LIST_MAX];
        if (l->bytes > sizeof list) wj_fatal(r, "gather list longer than supported", l->ea, l->bytes);
        if (l->bytes > l->limit || (l->bytes & 15)) wj_fatal(r, "gather list does not fit its buffer", l->bytes, l->limit);
        const uint32_t list_ls = l->ls + l->limit - l->bytes;
        wj_guest_read(r, l->ea, l->bytes, list, list_ls);
        memcpy(c->ls + list_ls, list, l->bytes);
        wwsjm_piece pc[sizeof list / 8];
        unsigned n = 0, overlap = 0;
        wwsjm_status st = wj_gather_plan(l, list, pc, sizeof pc / sizeof pc[0], &n, &overlap);
        if (overlap && st == WWSJM_OK) {
            WJ_COUNT(gather_overlaps, 1);
            if (r->check)
                fprintf(stderr, "[wwsjm] check: gather at 0x%08X (%u bytes) runs into its list copy at LS 0x%05X (wid %u)\n",
                        l->ea, l->bytes, list_ls, r->wid);
        }
        if (st != WWSJM_OK) {
            fprintf(stderr, "[wwsjm] refused gather list at 0x%08X (%u bytes) into LS 0x%05X, room 0x%X (list copy at 0x%05X), %s:",
                    l->ea, l->bytes, l->ls, l->limit, list_ls, wwsjm_status_name(st));
            for (uint32_t o = 0; o + 8 <= l->bytes; o += 8) fprintf(stderr, " (%X, %08X)", wwsjm_be32(list + o), wwsjm_be32(list + o + 4));
            fprintf(stderr, "\n");
            wj_fatal(r, "gather list outside the contract", l->ea, st);
        }
        for (unsigned k = 0; k < n; k++)
            wj_mfc(c, MFC_GET_CMD, pc[k].ls, pc[k].ea, pc[k].bytes, WJ_STAGE_TAG);
        issued |= n != 0;
        WJ_COUNT(gathers, 1);
    }
    if (issued) wj_wait_tag(c, WJ_STAGE_TAG);
}

/* SPU `bra` (absolute branch) to an LS address. */
static uint32_t wj_bra(uint32_t target) { return 0x30000000u | ((target >> 2) & 0xFFFFu) << 7; }

/* The fixed low local store every job may read (spec 13.4 item 1), planted
 * before each job: a job may have used any of it. The interrupt state at entry
 * is set separately, just before the job is entered (wj_run_job). */
static void wj_plant_low_ls(struct wj_run* r)
{
    uint8_t* ls = r->ctx->ls;
    for (unsigned i = 0; i < 4; i++) wwsjm_put32(ls + 4 * i, wj_bra(SPURS_NATIVE_PM_SLOT_LS(WJ_SVC_IRQ_LANDING)));
    memset(ls + 0x10, 0, WJ_JOB_SCRATCH_LS - 0x10);
    /* the SPURS kernel context, every word spec 13.4 lists */
    memset(ls + WJ_KCTX_LS, 0, 0xC0);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xC0, 0);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xC4, r->spurs_ea);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xC8, r->lane);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xCC, 8);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xD0, 0);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xD4, 0xA00);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xD8, r->wid);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xDC, r->wid);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xE0, SPURS_PM_EXIT_TO_KERNEL_LS);
    wwsjm_put32(ls + WJ_KCTX_LS + 0xE4, SPURS_PM_SELECT_WORKLOAD_LS);
}

static void wj_run_job(struct wj_run* r, const struct wj_job* job)
{
    spu_context* c = r->ctx;
    r->cur.valid = job->valid; r->cur.array = job->array; r->cur.index = job->index; r->cur.h = job->h;
    uint8_t rec[WWSJM_MAX_PORTION];
    wj_guest_read(r, job->h.ea, job->h.size, rec, WJ_PORTION_LS);
    /* planned, placed and checked before the claim (wj_preflight) */
    r->plan = job->plan;
    if (r->plan.nop_count) WJ_COUNT(nops, r->plan.nop_count);
    if (r->plan.set0_moved && __atomic_add_fetch(&s_wj_stats.set0_moved, 1, __ATOMIC_RELAXED) == 1)
        fprintf(stderr, "[wwsjm] set 0 reserved away from page %u (wid %u): allowed; the module may run interpreted\n",
                WWSJM_MODULE_PAGE, r->wid);
    if (!r->plan.set[0].reserved || !r->plan.set[0].pages) wj_fatal(r, "record without a module set", 0, 0);
    wwsjm_plan_residency(&r->plan, &r->res);
    wj_stage(r);

    /* the command portion, and the parameter data inside it */
    memcpy(c->ls + WJ_PORTION_LS, rec, job->h.size);
    wwsjm_context_args a;
    memset(&a, 0, sizeof a);
    a.lane = r->lane;
    a.dma_tag = 8;            /* the kernel's DMA tag (kernel context) */
    a.spurs_ea = r->spurs_ea;
    a.wid = r->wid;
    a.index = job->index;
    a.array = job->array;
    a.dep_count = r->plan.dep_count;
    a.list_ea = r->list_ea;
    a.list_tag = r->k_inst;
    a.userdata_ls = WJ_PORTION_LS + r->plan.userdata_offset;
    for (unsigned i = 0; i < WWSJM_SLOTS; i++) a.slot[i] = SPURS_NATIVE_PM_SLOT_LS(i);
    wwsjm_context_build(&a, c->ls + WJ_CTX_LS);

    uint32_t set0_ls, set0_bytes;
    wwsjm_buffer_span(&r->plan, 0, r->plan.set[0].first_buffer, &set0_ls, &set0_bytes);
    const uint32_t entry = set0_ls + wwsjm_be32(c->ls + set0_ls + 0x10);
    if (entry < set0_ls || entry >= set0_ls + set0_bytes || (entry & 3)) wj_fatal(r, "module entry outside its set", entry, set0_ls);

    wj_plant_low_ls(r);
    r->tags = 0;
    r->tag_n = 0;
    r->finishes = 0;
    r->released = 0;
    r->next.valid = 0;
    c->gpr[5] = spu_make_preferred_u32(entry);
    c->gpr[6] = spu_make_preferred_u32(WJ_ENTRY_R6);
    /* Interrupts enabled with no event enabled (spec 5.1, 13.4, 14): jobs
     * disable and re-enable them around service calls and may require them
     * enabled; with the mask zero none is ever taken. The job's own toggles
     * stand, and no service touches either. */
    c->event_mask = 0;
    c->int_enable = 1;
    WJ_COUNT(jobs, 1);
    const int entered = spurs_native_pm_enter_job(c, entry, WJ_STACK, WJ_STACK_BYTES, WJ_CTX_LS, 0);
    if (entered == SPURS_NATIVE_PM_JOB_HALTED) {
        /* the job halted or stopped: its entry stays claimed and unretired (its
         * lane marker too); the halt goes on to the run's own bracket */
        if (r->check)
            fprintf(stderr, "[wwsjm] check: job %u:0x%04X (wid %u lane %u, entry 0x%05X) halted with status 0x%X at pc 0x%05X;"
                    " it stays claimed and unretired\n", job->array, job->index, r->wid, r->lane, entry, c->status,
                    (uint32_t)c->pc & SPU_LS_MASK);
        spu_halt(c);
    }
    if (entered != 0)
        wj_fatal(r, "job did not return to the job-return slot", c->pc & SPU_LS_MASK, entry);
    if (r->check) {
        if (r->finishes != 1) wj_fatal(r, "check: finish not called exactly once", r->finishes, entry);
        if ((c->gpr[1]._u32[0] & SPU_LS_MASK) != WJ_STACK) wj_fatal(r, "check: stack not restored", c->gpr[1]._u32[0], WJ_STACK);
    }

    /* retire: the lane's marker moves to the job it still holds, or none */
    uint8_t old[128], now[128];
    wwsjm_held held = { r->next.valid, r->next.array, r->next.index };
    for (;;) {
        wj_getllar(r, WJ_LINE_LS, r->list_ea, old);
        if (wwsjm_retire(old, job->array, r->lane, &held, now) != WWSJM_OK) wj_fatal(r, "retire refused", job->array, r->lane);
        if (r->check) wj_check_line(r, old, now, job->array, 0);
        if (wj_putllc(r, WJ_LINE_LS, r->list_ea, now)) break;
    }
    WJ_COUNT(retires, 1);

    /* dependency counters, then the workloads they wake */
    for (unsigned d = 0; d < r->plan.dep_count; d++) {
        const uint32_t ea = r->plan.dep_ea[d], line_ea = ea & ~127u, off = ea & 127u;
        uint16_t v = 0;
        for (;;) {
            wj_getllar(r, WJ_DEP_LS, line_ea, old);
            if (wwsjm_decrement(old, off, now, &v) != WWSJM_OK) wj_fatal(r, "dependency counter offset", ea, off);
            if (wj_putllc(r, WJ_DEP_LS, line_ea, now)) break;
        }
        WJ_COUNT(decrements, 1);
        if (v) continue;
        uint8_t f[2];
        if (off + 2 < 128) memcpy(f, now + off + 2, 2);
        else wj_guest_read(r, line_ea + 128, 2, f, 0);
        unsigned target;
        uint8_t value;
        wwsjm_wake_target(wwsjm_be16(f), &target, &value);
        wj_ready_store(r, target, value);
        WJ_COUNT(wakes, 1);
    }
    wj_ready_rule(r);
    r->cur.valid = 0;
}

/* ---- the job services -------------------------------------------------------------- */

static struct wj_run* wj_current(spu_context* c)
{
    struct wj_run* r = t_wj;
    if (!r || r->ctx != c || !r->cur.valid) wj_fatal(r, "service called outside a native job", c->pc, 0);
    return r;
}

static int wj_svc_get_buffer(spu_context* c)
{
    struct wj_run* r = wj_current(c);
    const uint32_t set = c->gpr[4]._u32[0], buf = c->gpr[5]._u32[0], want = c->gpr[6]._u32[0];
    uint8_t tag = 0;
    /* one tag per (set, buffer) per job, taken at the pair's first r6 != 0
     * request (31 downward) and returned again for repeats; r6 = 0 gives 0 and
     * takes nothing (spec 5.3, 18.2) */
    if (want) {
        unsigned i = 0;
        while (i < r->tag_n && (r->tag_of[i].set != set || r->tag_of[i].buf != buf)) i++;
        if (i == r->tag_n) {
            if (i >= WJ_TAGS || wwsjm_job_tag(&r->tags, &tag) != WWSJM_OK)
                wj_fatal(r, "more DMA tags than a job may take", set, r->tags);
            r->tag_of[i].set = (uint8_t)set; r->tag_of[i].buf = (uint8_t)buf; r->tag_of[i].tag = tag;
            r->tag_n++;
        }
        tag = r->tag_of[i].tag;
    }
    uint8_t d[16];
    wwsjm_status st = wwsjm_buffer_descriptor(&r->plan, set, buf, tag, d);
    if (st != WWSJM_OK) wj_fatal(r, "get-buffer for a set the record did not reserve", set, buf);
    for (unsigned i = 0; i < 4; i++) c->gpr[3]._u32[i] = wwsjm_be32(d + 4 * i);
    return SPURS_NATIVE_SERVICE_RETURN;
}

static int wj_svc_release(spu_context* c)
{
    struct wj_run* r = wj_current(c);
    const uint32_t set = c->gpr[4]._u32[0];
    if (set < 32) r->released |= 1u << set;
    c->gpr[3]._u32[0] = c->gpr[3]._u32[1] = c->gpr[3]._u32[2] = c->gpr[3]._u32[3] = 0xFFFFFFFFu;
    return SPURS_NATIVE_SERVICE_RETURN;
}

/* Finish: poll, claim the next job if one is claimable (the lane's marker stays
 * on the running job), publish the ready count, return the claimed header. The
 * running job's completion is published after it returns. */
static int wj_svc_finish(spu_context* c)
{
    struct wj_run* r = wj_current(c);
    if (++r->finishes > 1 && r->check) wj_fatal(r, "check: finish called twice", r->finishes, 0);
    uint32_t result[2];
    wwsjm_finish_result(NULL, result);
    if (r->finishes == 1 && !r->stop) {
        if (wj_poll(r) != r->wid) r->stop = 1;
        else if (wj_claim(r, &r->next, 1)) wwsjm_finish_result(&r->next.h, result);
        wj_ready_rule(r);
    }
    c->gpr[3]._u32[0] = result[0];
    c->gpr[3]._u32[1] = result[1];
    c->gpr[3]._u32[2] = c->gpr[3]._u32[3] = 0;
    return SPURS_NATIVE_SERVICE_RETURN;
}

static int wj_svc_irq_landing(spu_context* c)
{
    wj_fatal(wj_current(c), "SPU interrupt vector entered (the native job manager raises no SPU events)",
             c->gpr[0]._u32[0] & SPU_LS_MASK, (uint32_t)c->pc & SPU_LS_MASK);
    return SPURS_NATIVE_SERVICE_RETURN;
}

static const struct spurs_native_services s_wj_services = { {
    [WJ_SVC_GET_BUFFER] = wj_svc_get_buffer,
    [WJ_SVC_FINISH] = wj_svc_finish,
    [WJ_SVC_RELEASE] = wj_svc_release,
    [WJ_SVC_IRQ_LANDING] = wj_svc_irq_landing,
} };

/* ---- one dispatch ---------------------------------------------------------------------- */

/* PS3RECOMP_WWSJM_STATS=<seconds>: the counters line every <seconds> (a process
 * that is killed never reaches the at-exit print). Off when unset. */
static void wj_periodic_stats(void)
{
    static int s_period = -1;
    static unsigned long long s_next;
    if (s_period < 0) {
        const char* e = getenv("PS3RECOMP_WWSJM_STATS");
        s_period = e && *e ? atoi(e) : 0;
    }
    if (s_period <= 0) return;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    const unsigned long long now = (unsigned long long)ts.tv_sec;
    unsigned long long next = __atomic_load_n(&s_next, __ATOMIC_RELAXED);
    if (!next) { __atomic_compare_exchange_n(&s_next, &next, now + (unsigned)s_period, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); return; }
    if (now >= next && __atomic_compare_exchange_n(&s_next, &next, now + (unsigned)s_period, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        wj_print_stats();
}

static int wj_run(spu_context* c, const struct spurs_native_pm_run* a)
{
    struct wj_run r;
    memset(&r, 0, sizeof r);
    r.ctx = c;
    r.wid = a->wid;
    r.lane = a->spu_num;
    r.spurs_ea = a->spurs_ea;
    r.list_ea = (uint32_t)a->wkl_data;
    r.check = a->mode == SPURS_NATIVE_PM_CHECK;
    r.fallback_ok = !r.check && a->lifted_available;
    AcquireSRWLockShared(&s_wj_lock);
    for (unsigned i = 0; i < sizeof s_wj_inst / sizeof s_wj_inst[0]; i++)
        if (s_wj_inst[i].spurs_ea == a->spurs_ea) r.busy = s_wj_inst[i].n_spus;
    ReleaseSRWLockShared(&s_wj_lock);
    /* Refused before anything is touched: the dispatch ends at once; the
     * workload is still ready, so its next dispatch runs the lifted module. */
    if (!r.busy) return wj_refuse(&r, "run on an instance the implementation did not accept", a->spurs_ea, 0);
    if (a->wkl_data >> 32) return wj_refuse(&r, "audit buffer requested (unsupported)", (uint32_t)(a->wkl_data >> 32), 0);
    if (r.list_ea & 127) return wj_refuse(&r, "job list not line aligned", r.list_ea, 0);
    r.k_inst = 0x0A000000u;   /* the context constant of the instance this implementation takes (spec U11) */
    if (r.lane >= WWSJM_LANES || r.wid >= 16) return wj_refuse(&r, "lane or workload out of range", r.lane, r.wid);
    wwsjm_residency_reset(&r.res);   /* every run reloads (spec R6) */
    struct wj_run* outer = t_wj;
    t_wj = &r;
    /* the interrupt state the run began with, restored when it ends */
    const uint32_t int_enable0 = c->int_enable, event_mask0 = c->event_mask;
    WJ_COUNT(runs, 1);
    const unsigned long long jobs_before = __atomic_load_n(&s_wj_stats.jobs, __ATOMIC_RELAXED);

    if (a->poll_status == 4) { wj_ready_store(&r, r.wid, WWSJM_READY_ON_FLAG); WJ_COUNT(flag_entries, 1); }
    for (;;) {
        struct wj_job job;
        if (r.next.valid) {
            job = r.next;
            r.next.valid = 0;
        } else if (r.stop) {
            break;
        } else if (!wj_claim(&r, &job, 0)) {
            if (r.withdraw) goto withdrawn;
            if (r.stop) break;
            /* nothing claimable on the claim's snapshot: ready := 0 (6.4), then
             * verify, and go on claiming if the verify finds work (6.5, R12) */
#ifdef WWSJM_TEST_HOOKS
            {   /* synthetic fixtures only: publish inside the exit window, after the
                 * decision and before ready := 0 (the window a lost kick needs) */
                extern void wwsjm_test_exit_window(uint32_t list_ea);
                wwsjm_test_exit_window(r.list_ea);
            }
#endif
            wj_ready_store(&r, r.wid, 0);
            if (wj_verify(&r)) continue;
            if (r.withdraw) goto withdrawn;
            goto verified;
        }
        wj_run_job(&r, &job);
    }
    /* the kernel asked for another workload: the ready value from a fresh
     * snapshot (R10); a 0 is verified, and work seen there is advertised */
    if (!r.withdraw && wj_ready_rule(&r) == 0 && wj_verify(&r)) wj_ready_store(&r, r.wid, (uint8_t)r.busy);
withdrawn:
    if (r.withdraw) {
        /* Withdrawn (wj_refuse): the run holds no job. The workload stays
         * ready, so the kernel dispatches it again, to the lifted module,
         * which takes the job list over from the guest state as it is. */
        wj_ready_store(&r, r.wid, (uint8_t)r.busy);
        WJ_COUNT(withdrawals, 1);
    }
verified:
    c->int_enable = int_enable0;
    c->event_mask = event_mask0;
#ifdef WWSJM_TEST_HOOKS
    { extern void wwsjm_test_run_end(uint32_t list_ea); wwsjm_test_run_end(r.list_ea); }
#endif
    if (__atomic_load_n(&s_wj_stats.jobs, __ATOMIC_RELAXED) == jobs_before) WJ_COUNT(idle_runs, 1);
    wj_periodic_stats();
    t_wj = outer;
    return 0;
}

/* The shadow comparison (PS3RECOMP_SPURS_NATIVE_PM=shadow). */
#include "wwsjm/wwsjm_shadow.inc"

/* Only the instance whose job-context constant and services the specification
 * establishes (five SPUs, constant 0x0A000000, three services); workloads of
 * other instances keep the lifted module (spec 1, U11, U12). */
static int wj_accept(uint32_t spurs_ea, uint32_t wid, uint32_t n_spus)
{
    (void)wid;
    if (n_spus != 5 || !spurs_ea) return 0;
    int ok = 0;
    AcquireSRWLockExclusive(&s_wj_lock);
    for (unsigned i = 0; i < sizeof s_wj_inst / sizeof s_wj_inst[0] && !ok; i++)
        if (s_wj_inst[i].spurs_ea == spurs_ea || !s_wj_inst[i].spurs_ea) {
            s_wj_inst[i].spurs_ea = spurs_ea;
            s_wj_inst[i].n_spus = n_spus;
            ok = 1;
        }
    ReleaseSRWLockExclusive(&s_wj_lock);
    return ok;
}

static const struct spurs_native_pm s_wj_impl = {
    "wwsjm", WJ_FINGERPRINT, { 0 }, wj_run, &s_wj_services, wj_accept, wj_shadow_event };

void spurs_wwsjm_print_stats(void) { wj_print_stats(); }

/* The implementation's descriptor (identity of the image the specification
 * describes). Fixtures register a copy under a manufactured image's identity. */
const struct spurs_native_pm* spurs_wwsjm_native(void) { return &s_wj_impl; }

void spurs_native_pm_register_builtins(void)
{
    static int s_done;
    if (__atomic_exchange_n(&s_done, 1, __ATOMIC_ACQ_REL)) return;
    static struct spurs_native_pm impl;
    impl = s_wj_impl;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) {
            const char ch = WJ_SHA256[2 * i + k];
            v = v * 16u + (unsigned)(ch <= '9' ? ch - '0' : ch - 'a' + 10);
        }
        impl.sha256[i] = (uint8_t)v;
    }
    if (spurs_native_pm_register(&impl) != 0) return;
    if (spurs_native_pm_mode() == SPURS_NATIVE_PM_SHADOW) atexit(spurs_wwsjm_shadow_print);
    else atexit(wj_print_stats);
}
