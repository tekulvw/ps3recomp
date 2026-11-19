/* spurs_pm_record.inc -- opt-in SPURS policy-run recorder and statistics.
 * Included once, by spurs_policy.c (so every program that links the policy
 * runner has it; no build list changes). Off unless one of these is set:
 *
 *   PS3RECOMP_SPURS_PM_RECORD=<dir>[:<wid list>][:<max runs>]
 *       one binary file per policy worker thread in <dir>
 *       (pmrec-<pid>-t<n>.bin; format: tools/spurs_pm_record.py), holding,
 *       for each recorded run: the entry (workload, lane, instance, workload
 *       data, poll status, the 128-byte line at the workload data's low word),
 *       every MFC command, every transfer element with a hash of its data
 *       and its first bytes, every lock-line operation with its line and
 *       status, every crossing between policy code and job code (job entries
 *       with the 128 bytes at r3, calls from a job into the policy module with
 *       r0/r1/r3-r6 and the context slot that held the target, and their
 *       returns with r3), the kernel services, and the exit.
 *       <wid list>: hex workload ids separated by commas (empty = all).
 *       <max runs>: recorded runs per (instance, workload), default unlimited.
 *       A Windows drive prefix (C:\...) is not taken for a suffix.
 *   PS3RECOMP_SPURS_PM_RECORD_BYTES=<n>   data bytes kept per transfer
 *       (default 4096; 0 keeps every byte). The hash always covers it all.
 *   PS3RECOMP_SPURS_PM_RECORD_START=<s>   record only runs that begin <s>
 *       seconds or more after the first policy run (default 0).
 *   PS3RECOMP_SPURS_PM_RECORD_EVERY=<n>   record one eligible run in n on each
 *       worker (default 1: consecutive runs).
 *   PS3RECOMP_SPURS_PM_STATS=1            print cumulative per-lane/workload
 *       counters and the inventory histograms to stderr ([spurs-pm-stats])
 *       every PS3RECOMP_SPURS_PM_STATS_SECONDS (default 5).
 *
 * The recorder only observes. The one structural effect is how a call from
 * job code into the policy module is dispatched while recording: the
 * observer resolves it and drains to the caller's r0 link itself so that it
 * sees the return (spurs_pm_rec_branch), which is the same dispatch and the
 * same drain target the lifted call bracket performs. With every variable
 * unset no context ever gets an observer and spu_run_policy_module pays one
 * load of a resolved configuration word per run. */
#include "spurs_pm_observer.h"
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define spurs_pm_rec_getpid() ((unsigned)_getpid())
#else
#include <sys/stat.h>
#include <unistd.h>
#define spurs_pm_rec_getpid() ((unsigned)getpid())
#endif

enum {
    SPURS_PM_REC_VERSION = 1,
    SPURS_PM_REC_LINE = 128,
    SPURS_PM_REC_CTX_BYTES = 128,
    SPURS_PM_REC_DEPTH = 16,
    SPURS_PM_REC_CANDIDATES = 64,
    SPURS_PM_REC_SLOTS = 48,
    SPURS_PM_REC_LIMITS = 64,
    SPURS_PM_REC_NO_SLOT = 0xFFFFFFFFu
};
/* Record kinds (tools/spurs_pm_record.py decodes these). */
enum {
    SPURS_PM_REC_RUN_BEGIN = 1, SPURS_PM_REC_RUN_END = 2, SPURS_PM_REC_COMMAND = 3,
    SPURS_PM_REC_TRANSFER = 4, SPURS_PM_REC_ATOMIC = 5, SPURS_PM_REC_BRANCH = 6,
    SPURS_PM_REC_KERNEL = 7
};
enum { SPURS_PM_REC_JOB_ENTER = 1, SPURS_PM_REC_JOB_RETURN = 2,
       SPURS_PM_REC_SERVICE_CALL = 3, SPURS_PM_REC_SERVICE_RETURN = 4 };
/* Where the observed program was when an event happened. */
enum { SPURS_PM_REGION_POLICY = 0, SPURS_PM_REGION_JOB = 1, SPURS_PM_REGION_SERVICE = 2 };

/* ---- configuration (read once) ----------------------------------------- */
static struct {
    int resolved;            /* 0 not read, 1 read */
    int any;                 /* recording or statistics configured */
    int record, stats;
    char dir[1024];
    uint32_t wid_mask;       /* bit w = record workload w */
    uint32_t max_runs;       /* 0 = unlimited */
    uint32_t byte_cap;       /* 0 = all */
    uint32_t every;
    uint64_t start_ns, stats_ns;
} s_pmrec;
static SRWLOCK s_pmrec_lock = SRWLOCK_INIT;

static uint64_t spurs_pm_rec_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static int spurs_pm_rec_is_dec(const char* s, size_t n)
{
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}
/* A wid list is hex ids separated by commas; the empty list means all. */
static int spurs_pm_rec_wids(const char* s, size_t n, uint32_t* mask)
{
    uint32_t m = 0; size_t i = 0;
    if (!n) { *mask = 0xFFFFu; return 1; }
    while (i < n) {
        unsigned v = 0; size_t d = 0;
        for (; i < n && s[i] != ','; i++, d++) {
            char c = s[i];
            int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (x < 0 || d >= 2) return 0;
            v = v * 16u + (unsigned)x;
        }
        if (!d || v >= 16) return 0;
        m |= 1u << v;
        if (i < n) { i++; if (i == n) return 0; }
    }
    *mask = m;
    return 1;
}
static uint32_t spurs_pm_rec_env_u32(const char* name, uint32_t dflt)
{
    const char* e = getenv(name);
    if (!e || !*e) return dflt;
    return (uint32_t)strtoul(e, NULL, 10);
}

static void spurs_pm_rec_print_stats(void);
static void spurs_pm_rec_configure(void)
{
    AcquireSRWLockExclusive(&s_pmrec_lock);
    if (!s_pmrec.resolved) {
        const char* r = getenv("PS3RECOMP_SPURS_PM_RECORD");
        const char* st = getenv("PS3RECOMP_SPURS_PM_STATS");
        s_pmrec.stats = st && *st && strcmp(st, "0") != 0;
        if (r && *r && strlen(r) < sizeof(s_pmrec.dir)) {
            /* <dir>[:<wids>][:<max>], parsed from the right so a drive letter
             * or a colon inside the directory never becomes a suffix. */
            size_t n = strlen(r);
            const char* c1 = NULL; const char* c2 = NULL;
            for (size_t i = n; i-- > 0;) if (r[i] == ':') { c2 = r + i; break; }
            if (c2) for (const char* p = c2; p-- > r;) if (*p == ':') { c1 = p; break; }
            uint32_t mask = 0xFFFFu, max = 0, m;
            size_t dir_len = n;
            const char* t2 = c2 ? c2 + 1 : NULL; size_t l2 = c2 ? (size_t)(r + n - t2) : 0;
            const char* t1 = c1 ? c1 + 1 : NULL; size_t l1 = c1 ? (size_t)(c2 - t1) : 0;
            if (c1 && spurs_pm_rec_wids(t1, l1, &m) && (l2 == 0 || spurs_pm_rec_is_dec(t2, l2))) {
                mask = m; max = l2 ? (uint32_t)strtoul(t2, NULL, 10) : 0; dir_len = (size_t)(c1 - r);
            } else if (c2 && spurs_pm_rec_wids(t2, l2, &m)) {
                mask = m; dir_len = (size_t)(c2 - r);
            }
            if (dir_len) {
                memcpy(s_pmrec.dir, r, dir_len); s_pmrec.dir[dir_len] = 0;
                s_pmrec.wid_mask = mask; s_pmrec.max_runs = max; s_pmrec.record = 1;
#ifdef _WIN32
                _mkdir(s_pmrec.dir);
#else
                mkdir(s_pmrec.dir, 0777);
#endif
            }
        }
        s_pmrec.byte_cap = spurs_pm_rec_env_u32("PS3RECOMP_SPURS_PM_RECORD_BYTES", 4096);
        s_pmrec.every = spurs_pm_rec_env_u32("PS3RECOMP_SPURS_PM_RECORD_EVERY", 1);
        if (!s_pmrec.every) s_pmrec.every = 1;
        s_pmrec.start_ns = (uint64_t)spurs_pm_rec_env_u32("PS3RECOMP_SPURS_PM_RECORD_START", 0) * 1000000000ull;
        s_pmrec.stats_ns = (uint64_t)spurs_pm_rec_env_u32("PS3RECOMP_SPURS_PM_STATS_SECONDS", 5) * 1000000000ull;
        if (!s_pmrec.stats_ns) s_pmrec.stats_ns = 5000000000ull;
        s_pmrec.any = s_pmrec.record || s_pmrec.stats;
        if (s_pmrec.stats) atexit(spurs_pm_rec_print_stats);
        if (s_pmrec.record)
            fprintf(stderr, "[spurs-pm-record] dir=%s wids=0x%04X max_runs=%u bytes=%u every=%u start_s=%llu\n",
                    s_pmrec.dir, s_pmrec.wid_mask, s_pmrec.max_runs, s_pmrec.byte_cap, s_pmrec.every,
                    (unsigned long long)(s_pmrec.start_ns / 1000000000ull));
        __atomic_store_n(&s_pmrec.resolved, 1, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&s_pmrec_lock);
}

/* ---- per-worker state ---------------------------------------------------- */
struct spurs_pm_rec_stats {
    uint64_t runs, idle_runs, recorded_runs, job_entries, job_returns, service_calls, polls, exits;
    uint64_t getllar, putllc_ok, putllc_fail, putlluc, commands, list_commands;
    uint64_t get_n, get_bytes, put_n, put_bytes, zero_n;
    uint64_t module_loads, module_bytes, instance_writes[2], irq_branches;
};
struct spurs_pm_rec_slot { uint32_t module_ea, offset; uint64_t calls; };
struct spurs_pm_rec_thread {
    struct spurs_pm_rec_thread* next;
    FILE* file;
    unsigned seq;
    uint64_t eligible;
    /* the current run */
    spu_context* ctx;
    int recording;
    uint32_t wid, lane, spurs_ea, pm_hi, run_seq, run_jobs, run_polls;
    uint64_t pm_fp; const uint8_t* pm_fp_host; uint32_t pm_fp_size;
    /* region tracking: base region plus nested service calls */
    int region;
    unsigned depth;
    struct { uint32_t host_depth; int region; } stack[SPURS_PM_REC_DEPTH];
    int bypass;
    uint32_t job_ctx_lsa, job_link, job_host_depth;
    /* the attached header arrays as last seen in the line */
    uint32_t array_line, array_ea[4], array_cap[4];
    struct { uint32_t ea, size; } cand[SPURS_PM_REC_CANDIDATES];
    unsigned cand_next;
    /* statistics, per workload; single writer (this worker), read by the printer */
    struct spurs_pm_rec_stats st[16];
    uint64_t entry_type[16];
    uint64_t opcode[8][32];
    struct spurs_pm_rec_slot slots[SPURS_PM_REC_SLOTS];
    uint64_t slot_overflow;
};
static struct spurs_pm_rec_thread* s_pmrec_threads;
static unsigned s_pmrec_thread_seq;
static SPU_THREAD_LOCAL struct spurs_pm_rec_thread* t_pmrec;
static uint64_t s_pmrec_first_ns, s_pmrec_next_print;
static uint32_t s_pmrec_run_seq;
static struct { uint32_t ea, wid, count; } s_pmrec_limits[SPURS_PM_REC_LIMITS];

#define PMREC_ADD(field, n) __atomic_store_n(&(field), __atomic_load_n(&(field), __ATOMIC_RELAXED) + (uint64_t)(n), __ATOMIC_RELAXED)

static struct spurs_pm_rec_thread* spurs_pm_rec_thread(void)
{
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (t) return t;
    t = (struct spurs_pm_rec_thread*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    AcquireSRWLockExclusive(&s_pmrec_lock);
    t->seq = s_pmrec_thread_seq++;
    t->next = s_pmrec_threads;
    s_pmrec_threads = t;
    ReleaseSRWLockExclusive(&s_pmrec_lock);
    t_pmrec = t;
    return t;
}

static uint32_t spurs_pm_rec_be32(const uint8_t* p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t spurs_pm_rec_fnv(const uint8_t* p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- file output (little-endian fields) ---------------------------------- */
struct spurs_pm_rec_buf { uint8_t b[512]; uint32_t n; };
static void pmb_u32(struct spurs_pm_rec_buf* o, uint32_t v)
{ for (int i = 0; i < 4; i++) o->b[o->n++] = (uint8_t)(v >> (8 * i)); }
static void pmb_u64(struct spurs_pm_rec_buf* o, uint64_t v)
{ pmb_u32(o, (uint32_t)v); pmb_u32(o, (uint32_t)(v >> 32)); }
static void pmb_bytes(struct spurs_pm_rec_buf* o, const uint8_t* p, uint32_t n)
{ if (p) memcpy(o->b + o->n, p, n); else memset(o->b + o->n, 0, n); o->n += n; }

static void spurs_pm_rec_emit(struct spurs_pm_rec_thread* t, uint32_t kind,
                              const struct spurs_pm_rec_buf* body,
                              const uint8_t* tail, uint32_t tail_n)
{
    if (!t->file) return;
    uint8_t h[8] = { (uint8_t)kind, 0, 0, 0 };
    uint32_t len = body->n + tail_n;
    for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)(len >> (8 * i));
    fwrite(h, 1, 8, t->file);
    fwrite(body->b, 1, body->n, t->file);
    if (tail_n) fwrite(tail, 1, tail_n, t->file);
}

static int spurs_pm_rec_open(struct spurs_pm_rec_thread* t)
{
    if (t->file) return 1;
    char path[1200];
    snprintf(path, sizeof(path), "%s/pmrec-%u-t%u.bin", s_pmrec.dir, spurs_pm_rec_getpid(), t->seq);
    t->file = fopen(path, "wb");
    if (!t->file) {
        fprintf(stderr, "[spurs-pm-record] cannot open %s (errno %d): recording off on this worker\n", path, errno);
        return 0;
    }
    setvbuf(t->file, NULL, _IOFBF, 1 << 20);
    struct spurs_pm_rec_buf b = { {0}, 0 };
    pmb_bytes(&b, (const uint8_t*)"SPMR", 4);
    pmb_u32(&b, SPURS_PM_REC_VERSION);
    pmb_u32(&b, 32);
    pmb_u32(&b, s_pmrec.byte_cap);
    pmb_u32(&b, spurs_pm_rec_getpid());
    pmb_u32(&b, t->seq);
    pmb_u64(&b, spurs_pm_rec_now());
    fwrite(b.b, 1, b.n, t->file);
    return 1;
}

/* ---- guest views ---------------------------------------------------------- */
/* The 128 bytes at a guest EA, without side effects; 0 if not readable. */
static int spurs_pm_rec_guest_line(uint32_t ea, uint8_t* out)
{
    extern uint8_t g_vm_page_bitmap[65536 / 8];
    uint64_t limit = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    if (!vm_base || !ea || (ea & 127u) || (uint64_t)ea + SPURS_PM_REC_LINE > limit) return 0;
    if (!((g_vm_page_bitmap[(ea >> 16) >> 3] >> ((ea >> 16) & 7)) & 1)) return 0;
    memcpy(out, vm_base + ea, SPURS_PM_REC_LINE);
    return 1;
}
/* Header arrays named by a job-list line (spec: capacity-1 at +0x20+4a, EA at
 * +0x30+4a). Only used to classify reads for the statistics. */
static void spurs_pm_rec_note_line(struct spurs_pm_rec_thread* t, const uint8_t* line)
{
    for (int a = 0; a < 4; a++) {
        t->array_cap[a] = spurs_pm_rec_be32(line + 0x20 + 4 * a);
        t->array_ea[a] = spurs_pm_rec_be32(line + 0x30 + 4 * a);
    }
}
static void spurs_pm_rec_branch_event(struct spurs_pm_rec_thread* t, struct spu_context* ctx, uint32_t kind,
                                      uint32_t pc, uint32_t slot, uint32_t module_ea, uint32_t how,
                                      const uint8_t* extra, uint32_t extra_n);
static void spurs_pm_rec_unwind(struct spurs_pm_rec_thread* t, spu_context* ctx)
{
    /* A host frame of a nested call was abandoned (an interrupt return or a
     * halt unwound past it): forget the nesting it held. */
    while (t->depth && ctx->host_depth < t->stack[t->depth - 1].host_depth)
        t->region = t->stack[--t->depth].region;
    /* A job entered from a call bracket returns by leaving that host frame,
     * which no resolver sees: the frame depth falling below the entry depth
     * says the job has returned. */
    if (!t->depth && t->region == SPURS_PM_REGION_JOB && ctx->host_depth < t->job_host_depth) {
        t->region = SPURS_PM_REGION_POLICY;
        PMREC_ADD(t->st[t->wid].job_returns, 1);
        spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_JOB_RETURN, ctx->pc & SPU_LS_MASK,
                                  SPURS_PM_REC_NO_SLOT, 0, 1, NULL, 0);
    }
}

/* ---- statistics decoders (spec-level interface facts only) ---------------- */
static void spurs_pm_rec_stat_get(struct spurs_pm_rec_thread* t, uint64_t ea64, uint32_t size,
                                  const uint8_t* bytes)
{
    const uint32_t ea = (uint32_t)ea64;
    /* job headers: 8 bytes each, entry type in bytes 2-3, record EA in 4-7 */
    for (int a = 0; a < 4; a++) {
        const uint32_t base = t->array_ea[a];
        if (!base) continue;
        const uint64_t span = 8ull * ((uint64_t)t->array_cap[a] + 1u);
        if (ea < base || (uint64_t)ea + size > base + span) continue;
        for (uint32_t o = (8u - ((ea - base) & 7u)) & 7u; o + 8 <= size; o += 8) {
            const uint8_t* h = bytes + o;
            const uint32_t type = ((uint32_t)h[2] << 8 | h[3]) & 15u;
            PMREC_ADD(t->entry_type[type], 1);
            if (type == 1) {
                unsigned k = t->cand_next++ % SPURS_PM_REC_CANDIDATES;
                t->cand[k].ea = spurs_pm_rec_be32(h + 4);
                t->cand[k].size = ((uint32_t)h[0] << 8 | h[1]) & 0xFFFu;
            }
        }
        return;
    }
    /* a job record's command portion: 8-byte commands, command word 0 first */
    for (unsigned k = 0; k < SPURS_PM_REC_CANDIDATES; k++) {
        if (!t->cand[k].ea || t->cand[k].ea != ea) continue;
        uint32_t n = t->cand[k].size < size ? t->cand[k].size : size;
        for (uint32_t o = 0; o + 8 <= n; o += 8) {
            const uint32_t w = spurs_pm_rec_be32(bytes + o);
            PMREC_ADD(t->opcode[(w >> 9) & 7u][(w >> 12) & 31u], 1);
        }
        t->cand[k].ea = 0;
        return;
    }
    /* a job module image: magic at +0x20, image size at +0x14 */
    if (size >= 0x30 && spurs_pm_rec_be32(bytes + 0x20) == 0xC0DEC0DEu) {
        PMREC_ADD(t->st[t->wid].module_loads, 1);
        PMREC_ADD(t->st[t->wid].module_bytes, spurs_pm_rec_be32(bytes + 0x14));
    }
}

/* ---- observer hooks -------------------------------------------------------- */
static void spurs_pm_rec_transfer(spu_context* ctx, uint32_t kind, uint32_t cmd, uint32_t lsa,
                                  uint64_t ea, uint32_t size, const uint8_t* bytes)
{
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (!t || t->ctx != ctx) return;
    spurs_pm_rec_unwind(t, ctx);
    struct spurs_pm_rec_stats* s = &t->st[t->wid];
    if (kind == SPURS_PM_XFER_GET) {
        PMREC_ADD(s->get_n, 1); PMREC_ADD(s->get_bytes, size);
        if (t->region != SPURS_PM_REGION_JOB && bytes) spurs_pm_rec_stat_get(t, ea, size, bytes);
    } else if (kind == SPURS_PM_XFER_PUT) {
        PMREC_ADD(s->put_n, 1); PMREC_ADD(s->put_bytes, size);
    } else PMREC_ADD(s->zero_n, 1);
    if (kind != SPURS_PM_XFER_GET && t->region != SPURS_PM_REGION_JOB && t->spurs_ea) {
        uint64_t off = ea - t->spurs_ea;
        if (ea >= t->spurs_ea && off < 0x2000) PMREC_ADD(s->instance_writes[off >= 0x1000], 1);
    }
    if (!t->recording) return;
    uint32_t keep = bytes ? size : 0;
    if (s_pmrec.byte_cap && keep > s_pmrec.byte_cap) keep = s_pmrec.byte_cap;
    struct spurs_pm_rec_buf b; b.n = 0;
    pmb_u32(&b, kind); pmb_u32(&b, cmd); pmb_u32(&b, lsa); pmb_u32(&b, size);
    pmb_u64(&b, ea); pmb_u64(&b, bytes ? spurs_pm_rec_fnv(bytes, size) : 0);
    pmb_u32(&b, (uint32_t)t->region); pmb_u32(&b, t->depth); pmb_u32(&b, keep);
    spurs_pm_rec_emit(t, SPURS_PM_REC_TRANSFER, &b, bytes, keep);
}

static void spurs_pm_rec_command(spu_context* ctx, uint32_t cmd, uint32_t lsa, uint64_t ea,
                                 uint32_t size, uint32_t tag)
{
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (!t || t->ctx != ctx) return;
    spurs_pm_rec_unwind(t, ctx);
    PMREC_ADD(t->st[t->wid].commands, 1);
    if (cmd & 0x04u) PMREC_ADD(t->st[t->wid].list_commands, 1);   /* MFC list forms */
    if (!t->recording) return;
    struct spurs_pm_rec_buf b; b.n = 0;
    pmb_u32(&b, cmd); pmb_u32(&b, lsa); pmb_u64(&b, ea); pmb_u32(&b, size); pmb_u32(&b, tag);
    pmb_u32(&b, (uint32_t)t->region); pmb_u32(&b, t->depth);
    spurs_pm_rec_emit(t, SPURS_PM_REC_COMMAND, &b, NULL, 0);
}

static void spurs_pm_rec_atomic(spu_context* ctx, uint32_t cmd, uint32_t lsa, uint32_t ea,
                                const uint8_t* line, uint32_t status)
{
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (!t || t->ctx != ctx) return;
    spurs_pm_rec_unwind(t, ctx);
    struct spurs_pm_rec_stats* s = &t->st[t->wid];
    if (cmd == MFC_GETLLAR_CMD) {
        PMREC_ADD(s->getllar, 1);
        /* the workload's own line: keep the attached header arrays current */
        if (t->region != SPURS_PM_REGION_JOB && ea == t->array_line) spurs_pm_rec_note_line(t, line);
    } else if (cmd == MFC_PUTLLC_CMD) {
        if (status == 0) PMREC_ADD(s->putllc_ok, 1); else PMREC_ADD(s->putllc_fail, 1);
    } else PMREC_ADD(s->putlluc, 1);
    if (cmd != MFC_GETLLAR_CMD && t->region != SPURS_PM_REGION_JOB && t->spurs_ea &&
        ea >= t->spurs_ea && ea - t->spurs_ea < 0x2000)
        PMREC_ADD(s->instance_writes[ea - t->spurs_ea >= 0x1000], 1);
    if (!t->recording) return;
    struct spurs_pm_rec_buf b; b.n = 0;
    pmb_u32(&b, cmd); pmb_u32(&b, lsa); pmb_u32(&b, ea); pmb_u32(&b, status);
    pmb_u32(&b, (uint32_t)t->region); pmb_u32(&b, t->depth);
    pmb_bytes(&b, line, SPURS_PM_REC_LINE);
    spurs_pm_rec_emit(t, SPURS_PM_REC_ATOMIC, &b, NULL, 0);
}

static void spurs_pm_rec_branch_event(struct spurs_pm_rec_thread* t, spu_context* ctx, uint32_t kind,
                                      uint32_t pc, uint32_t slot, uint32_t module_ea, uint32_t how,
                                      const uint8_t* extra, uint32_t extra_n)
{
    if (!t->recording) return;
    struct spurs_pm_rec_buf b; b.n = 0;
    pmb_u32(&b, kind); pmb_u32(&b, pc);
    pmb_u32(&b, ctx->gpr[0]._u32[0]); pmb_u32(&b, ctx->gpr[1]._u32[0]);
    for (int i = 0; i < 4; i++) pmb_u32(&b, ctx->gpr[3]._u32[i]);
    pmb_u32(&b, ctx->gpr[4]._u32[0]); pmb_u32(&b, ctx->gpr[5]._u32[0]); pmb_u32(&b, ctx->gpr[6]._u32[0]);
    pmb_u32(&b, t->depth); pmb_u32(&b, slot); pmb_u32(&b, module_ea); pmb_u32(&b, t->job_ctx_lsa);
    pmb_u32(&b, how); pmb_u32(&b, extra_n);
    spurs_pm_rec_emit(t, SPURS_PM_REC_BRANCH, &b, extra, extra_n);
}

/* The verified/streamed code region holding an LS address: its source EA. */
static uint32_t spurs_pm_rec_module_of(const spu_context* ctx, uint32_t lsa)
{
    for (int i = 0; i < 4; i++) {
        uint32_t lo = ctx->resident_code[i].lsa, n = ctx->resident_code[i].size;
        if (n && lsa >= lo && lsa - lo < n) return ctx->resident_code[i].source_ea;
    }
    return 0;
}
static void spurs_pm_rec_note_slot(struct spurs_pm_rec_thread* t, uint32_t module_ea, uint32_t offset)
{
    for (int i = 0; i < SPURS_PM_REC_SLOTS; i++) {
        struct spurs_pm_rec_slot* e = &t->slots[i];
        if (e->calls && (e->module_ea != module_ea || e->offset != offset)) continue;
        if (!e->calls) { e->module_ea = module_ea; e->offset = offset; }
        PMREC_ADD(e->calls, 1);
        return;
    }
    PMREC_ADD(t->slot_overflow, 1);
}

extern void spu_indirect_branch(spu_context*);
extern void spu_drain_call(spu_context*, uint32_t);

static int spurs_pm_rec_branch(spu_context* ctx)
{
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (!t || t->ctx != ctx) return 0;
    if (t->bypass) { t->bypass = 0; return 0; }   /* our own nested resolution */
    const uint32_t pc = ctx->pc & SPU_LS_MASK;
    if (pc == SPURS_PM_EXIT_TO_KERNEL_LS || pc == SPURS_PM_SELECT_WORKLOAD_LS) return 0;
    /* Inside an SPU interrupt handler (registers saved until its return) the
     * link register still holds whatever the interrupted code had: branches
     * there are the handler's own, never a crossing. Not classified. */
    if (ctx->irq_saved) { PMREC_ADD(t->st[t->wid].irq_branches, 1); return 0; }
    spurs_pm_rec_unwind(t, ctx);
    const uint32_t link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    const int to_job = pc >= t->pm_hi, from_job = link >= t->pm_hi;
    if (to_job && !from_job) {
        /* policy code enters job code; r0 is where the job returns. Policy code
         * running again at this level means the previous job has returned. */
        if (!t->depth && t->region == SPURS_PM_REGION_JOB) {
            PMREC_ADD(t->st[t->wid].job_returns, 1);
            spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_JOB_RETURN, pc, SPURS_PM_REC_NO_SLOT, 0, 1, NULL, 0);
        }
        uint8_t block[SPURS_PM_REC_CTX_BYTES];
        const uint32_t r3 = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
        uint32_t n = SPU_LS_SIZE - r3 < SPURS_PM_REC_CTX_BYTES ? SPU_LS_SIZE - r3 : SPURS_PM_REC_CTX_BYTES;
        memcpy(block, ctx->ls + r3, n);
        t->region = SPURS_PM_REGION_JOB;
        t->job_ctx_lsa = r3; t->job_link = link; t->job_host_depth = ctx->host_depth; t->run_jobs++;
        PMREC_ADD(t->st[t->wid].job_entries, 1);
        spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_JOB_ENTER, pc, SPURS_PM_REC_NO_SLOT,
                                  spurs_pm_rec_module_of(ctx, pc), 0, block, n);
        return 0;
    }
    if (!to_job && from_job) {
        /* job code calls into the policy module (a service): r0 is its link */
        uint32_t slot = SPURS_PM_REC_NO_SLOT;
        if (t->job_ctx_lsa)
            for (uint32_t o = 0; o + 4 <= SPURS_PM_REC_CTX_BYTES && t->job_ctx_lsa + o + 4 <= SPU_LS_SIZE; o += 4)
                if ((spurs_pm_rec_be32(ctx->ls + t->job_ctx_lsa + o) & SPU_LS_MASK) == pc) { slot = o; break; }
        const uint32_t module_ea = spurs_pm_rec_module_of(ctx, link);
        PMREC_ADD(t->st[t->wid].service_calls, 1);
        spurs_pm_rec_note_slot(t, module_ea, slot);
        spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_SERVICE_CALL, pc, slot, module_ea, 0, NULL, 0);
        if (t->depth >= SPURS_PM_REC_DEPTH) return 0;
        t->stack[t->depth].host_depth = ctx->host_depth;
        t->stack[t->depth].region = t->region;
        t->depth++;
        t->region = SPURS_PM_REGION_SERVICE;
        t->bypass = 1;
        spu_indirect_branch(ctx);      /* the resolution the caller asked for */
        spu_drain_call(ctx, link);     /* and the drain its call bracket runs */
        spurs_pm_rec_unwind(t, ctx);
        if (t->depth && t->stack[t->depth - 1].host_depth == ctx->host_depth) {
            t->region = t->stack[--t->depth].region;
            spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_SERVICE_RETURN, ctx->pc & SPU_LS_MASK,
                                      slot, module_ea, 0, NULL, 0);
        }
        return 1;
    }
    if (!to_job && t->region == SPURS_PM_REGION_JOB && pc == t->job_link) {
        /* a job returned to its link through the resolver */
        t->region = SPURS_PM_REGION_POLICY;
        PMREC_ADD(t->st[t->wid].job_returns, 1);
        spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_JOB_RETURN, pc, SPURS_PM_REC_NO_SLOT, 0, 0, NULL, 0);
    }
    return 0;
}

static void spurs_pm_rec_kernel(spu_context* ctx, uint32_t kind)
{
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (!t || t->ctx != ctx) return;
    spurs_pm_rec_unwind(t, ctx);
    if (kind == SPURS_PM_KERNEL_SELECT) { t->run_polls++; PMREC_ADD(t->st[t->wid].polls, 1); }
    else PMREC_ADD(t->st[t->wid].exits, 1);
    if (!t->recording) return;
    struct spurs_pm_rec_buf b; b.n = 0;
    pmb_u32(&b, kind);
    for (int i = 0; i < 4; i++) pmb_u32(&b, ctx->gpr[3]._u32[i]);
    pmb_u32(&b, (uint32_t)t->region); pmb_u32(&b, t->depth); pmb_u32(&b, t->run_polls);
    pmb_u32(&b, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    spurs_pm_rec_emit(t, SPURS_PM_REC_KERNEL, &b, NULL, 0);
}

static const struct spurs_pm_observer s_pmrec_observer = {
    spurs_pm_rec_transfer, spurs_pm_rec_command, spurs_pm_rec_atomic,
    spurs_pm_rec_branch, spurs_pm_rec_kernel
};

/* ---- run bracket ------------------------------------------------------------ */
static int spurs_pm_rec_take_run(uint32_t spurs_ea, uint32_t wid)
{
    if (!(s_pmrec.wid_mask & (1u << wid))) return 0;
    if (!s_pmrec.max_runs) return 1;
    int ok = 0;
    AcquireSRWLockExclusive(&s_pmrec_lock);
    for (int i = 0; i < SPURS_PM_REC_LIMITS; i++) {
        if (s_pmrec_limits[i].count && (s_pmrec_limits[i].ea != spurs_ea || s_pmrec_limits[i].wid != wid))
            continue;
        s_pmrec_limits[i].ea = spurs_ea; s_pmrec_limits[i].wid = wid;
        if (s_pmrec_limits[i].count < s_pmrec.max_runs) { s_pmrec_limits[i].count++; ok = 1; }
        break;
    }
    ReleaseSRWLockExclusive(&s_pmrec_lock);
    return ok;
}

static void spurs_pm_record_run_begin(spu_context* ctx, int image_id, const uint8_t* pm_host,
                                      uint32_t pm_size, uint64_t wkl_data, uint32_t wid,
                                      uint32_t spurs_ea, uint32_t spu_num, int first)
{
    if (!__atomic_load_n(&s_pmrec.resolved, __ATOMIC_ACQUIRE)) spurs_pm_rec_configure();
    if (!s_pmrec.any) return;
    struct spurs_pm_rec_thread* t = spurs_pm_rec_thread();
    if (!t) return;
    const uint64_t now = spurs_pm_rec_now();
    uint64_t first_ns = __atomic_load_n(&s_pmrec_first_ns, __ATOMIC_RELAXED);
    if (!first_ns) {
        uint64_t zero = 0;
        __atomic_compare_exchange_n(&s_pmrec_first_ns, &zero, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        first_ns = __atomic_load_n(&s_pmrec_first_ns, __ATOMIC_RELAXED);
    }
    t->ctx = ctx; t->wid = wid & 15u; t->lane = spu_num; t->spurs_ea = spurs_ea;
    t->pm_hi = 0xA00u + pm_size;
    t->region = SPURS_PM_REGION_POLICY; t->depth = 0; t->bypass = 0;
    t->job_ctx_lsa = 0; t->job_link = 0; t->job_host_depth = 0; t->run_jobs = 0; t->run_polls = 0;
    t->array_line = (uint32_t)wkl_data;
    memset(t->array_ea, 0, sizeof(t->array_ea));
    memset(t->cand, 0, sizeof(t->cand));
    uint8_t line[SPURS_PM_REC_LINE];
    const int line_ok = spurs_pm_rec_guest_line((uint32_t)wkl_data, line);
    if (line_ok) spurs_pm_rec_note_line(t, line);
    PMREC_ADD(t->st[t->wid].runs, 1);
    t->recording = 0;
    if (s_pmrec.record && now - first_ns >= s_pmrec.start_ns && (s_pmrec.wid_mask & (1u << t->wid)) &&
        (t->eligible++ % s_pmrec.every) == 0 && spurs_pm_rec_take_run(spurs_ea, t->wid) &&
        spurs_pm_rec_open(t))
        t->recording = 1;
    if (t->recording) {
        PMREC_ADD(t->st[t->wid].recorded_runs, 1);
        t->run_seq = __atomic_add_fetch(&s_pmrec_run_seq, 1, __ATOMIC_RELAXED);
        if (t->pm_fp_host != pm_host || t->pm_fp_size != pm_size) {
            t->pm_fp = spu_workload_fingerprint(pm_host, pm_size);
            t->pm_fp_host = pm_host; t->pm_fp_size = pm_size;
        }
        uint32_t pm_ea = 0;
        if (vm_base && pm_host >= vm_base && (uint64_t)(pm_host - vm_base) < UINT64_C(0x100000000))
            pm_ea = (uint32_t)(pm_host - vm_base);
        struct spurs_pm_rec_buf b; b.n = 0;
        pmb_u32(&b, t->run_seq); pmb_u32(&b, wid); pmb_u32(&b, spu_num); pmb_u32(&b, spurs_ea);
        pmb_u64(&b, wkl_data); pmb_u32(&b, ctx->gpr[5]._u32[0]); pmb_u32(&b, (uint32_t)image_id);
        pmb_u32(&b, pm_ea); pmb_u32(&b, pm_size); pmb_u64(&b, t->pm_fp);
        pmb_u32(&b, (uint32_t)first); pmb_u32(&b, (uint32_t)line_ok); pmb_u64(&b, now);
        pmb_bytes(&b, line_ok ? line : NULL, SPURS_PM_REC_LINE);
        spurs_pm_rec_emit(t, SPURS_PM_REC_RUN_BEGIN, &b, NULL, 0);
    }
    ctx->pm_observer = &s_pmrec_observer;
}

static void spurs_pm_rec_print_stats(void)
{
    static const char* const op_names[8] = { "nop", "reserve", "use", "unreserve", "dep-dec", "run", "end", "invalid" };
    AcquireSRWLockShared(&s_pmrec_lock);
    struct spurs_pm_rec_thread* list = s_pmrec_threads;
    uint64_t types[16] = {0}, ops[8][32] = {{0}};
    fprintf(stderr, "[spurs-pm-stats] t_s=%.1f\n",
            (double)(spurs_pm_rec_now() - __atomic_load_n(&s_pmrec_first_ns, __ATOMIC_RELAXED)) / 1e9);
    for (struct spurs_pm_rec_thread* t = list; t; t = t->next) {
#define L(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
        for (int w = 0; w < 16; w++) {
            struct spurs_pm_rec_stats* s = &t->st[w];
            if (!L(s->runs)) continue;
            fprintf(stderr, "[spurs-pm-stats] worker=%u inst=0x%X lane=%u wid=%X runs=%llu idle=%llu recorded=%llu "
                    "jobs=%llu job_returns=%llu services=%llu polls=%llu exits=%llu getllar=%llu putllc_ok=%llu "
                    "putllc_fail=%llu putlluc=%llu cmds=%llu list_cmds=%llu get=%llu/%lluB put=%llu/%lluB zero=%llu "
                    "module_loads=%llu module_bytes=%llu instance_writes=%llu/%llu irq_branches=%llu\n",
                    t->seq, t->spurs_ea, t->lane, w,
                    (unsigned long long)L(s->runs), (unsigned long long)L(s->idle_runs),
                    (unsigned long long)L(s->recorded_runs), (unsigned long long)L(s->job_entries),
                    (unsigned long long)L(s->job_returns), (unsigned long long)L(s->service_calls),
                    (unsigned long long)L(s->polls), (unsigned long long)L(s->exits),
                    (unsigned long long)L(s->getllar), (unsigned long long)L(s->putllc_ok),
                    (unsigned long long)L(s->putllc_fail), (unsigned long long)L(s->putlluc),
                    (unsigned long long)L(s->commands), (unsigned long long)L(s->list_commands),
                    (unsigned long long)L(s->get_n), (unsigned long long)L(s->get_bytes),
                    (unsigned long long)L(s->put_n), (unsigned long long)L(s->put_bytes),
                    (unsigned long long)L(s->zero_n), (unsigned long long)L(s->module_loads),
                    (unsigned long long)L(s->module_bytes), (unsigned long long)L(s->instance_writes[0]),
                    (unsigned long long)L(s->instance_writes[1]), (unsigned long long)L(s->irq_branches));
        }
        for (int i = 0; i < 16; i++) types[i] += L(t->entry_type[i]);
        for (int o = 0; o < 8; o++) for (int f = 0; f < 32; f++) ops[o][f] += L(t->opcode[o][f]);
    }
    fprintf(stderr, "[spurs-pm-stats] entry-types");
    for (int i = 0; i < 16; i++) if (types[i]) fprintf(stderr, " %d:%llu", i, (unsigned long long)types[i]);
    fprintf(stderr, "\n[spurs-pm-stats] commands (opcode flags=cadw,read,gather,oshare,swid)");
    for (int o = 0; o < 8; o++) for (int f = 0; f < 32; f++)
        if (ops[o][f]) fprintf(stderr, " %s/%d%d%d%d%d:%llu", op_names[o], (f >> 4) & 1, (f >> 3) & 1,
                               (f >> 2) & 1, (f >> 1) & 1, f & 1, (unsigned long long)ops[o][f]);
    fprintf(stderr, "\n");
    /* service slots, merged by (module, context offset) */
    struct spurs_pm_rec_slot merged[SPURS_PM_REC_SLOTS * 2]; int nm = 0; uint64_t over = 0;
    for (struct spurs_pm_rec_thread* t = list; t; t = t->next) {
        over += L(t->slot_overflow);
        for (int i = 0; i < SPURS_PM_REC_SLOTS; i++) {
            uint64_t c = L(t->slots[i].calls);
            if (!c) continue;
            int j = 0;
            for (; j < nm; j++) if (merged[j].module_ea == t->slots[i].module_ea && merged[j].offset == t->slots[i].offset) break;
            if (j == nm) { if (nm == SPURS_PM_REC_SLOTS * 2) { over += c; continue; } merged[nm++] = t->slots[i]; merged[j].calls = 0; }
            merged[j].calls += c;
        }
    }
#undef L
    fprintf(stderr, "[spurs-pm-stats] service-slots (module_ea/context_offset)");
    for (int j = 0; j < nm; j++) {
        if (merged[j].offset == SPURS_PM_REC_NO_SLOT)
            fprintf(stderr, " %08X/none:%llu", merged[j].module_ea, (unsigned long long)merged[j].calls);
        else
            fprintf(stderr, " %08X/+0x%X:%llu", merged[j].module_ea, merged[j].offset, (unsigned long long)merged[j].calls);
    }
    if (over) fprintf(stderr, " overflow:%llu", (unsigned long long)over);
    fprintf(stderr, "\n");
    fflush(stderr);
    ReleaseSRWLockShared(&s_pmrec_lock);
}

static void spurs_pm_record_run_end(spu_context* ctx, int result)
{
    if (!ctx->pm_observer) return;
    ctx->pm_observer = NULL;
    struct spurs_pm_rec_thread* t = t_pmrec;
    if (!t || t->ctx != ctx) return;
    if (!t->run_jobs) PMREC_ADD(t->st[t->wid].idle_runs, 1);
    if (t->recording) {
        extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_polls, g_spurs_pm_exited;
        uint8_t line[SPURS_PM_REC_LINE];
        const int line_ok = spurs_pm_rec_guest_line(t->array_line, line);
        struct spurs_pm_rec_buf b; b.n = 0;
        pmb_u32(&b, t->run_seq); pmb_u32(&b, (uint32_t)result); pmb_u32(&b, g_spurs_pm_polls);
        pmb_u32(&b, g_spurs_pm_exited); pmb_u32(&b, ctx->status); pmb_u32(&b, ctx->pc & SPU_LS_MASK);
        pmb_u32(&b, t->run_jobs); pmb_u32(&b, (uint32_t)line_ok); pmb_u64(&b, spurs_pm_rec_now());
        pmb_bytes(&b, line_ok ? line : NULL, SPURS_PM_REC_LINE);
        spurs_pm_rec_emit(t, SPURS_PM_REC_RUN_END, &b, NULL, 0);
        fflush(t->file);
        t->recording = 0;
    }
    t->ctx = NULL;
    if (s_pmrec.stats) {
        const uint64_t now = spurs_pm_rec_now();
        uint64_t next = __atomic_load_n(&s_pmrec_next_print, __ATOMIC_RELAXED);
        if (!next) {
            __atomic_compare_exchange_n(&s_pmrec_next_print, &next, now + s_pmrec.stats_ns, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        } else if (now >= next &&
                   __atomic_compare_exchange_n(&s_pmrec_next_print, &next, now + s_pmrec.stats_ns, 0,
                                               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            spurs_pm_rec_print_stats();
        }
    }
}
