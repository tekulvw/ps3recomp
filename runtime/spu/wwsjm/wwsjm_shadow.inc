/* wwsjm_shadow.inc -- shadow comparison for the native WWS job manager
 * (PS3RECOMP_SPURS_NATIVE_PM=shadow). Included by wwsjm_run.inc.
 *
 * The lifted policy module runs and owns every effect. The policy-run
 * recorder (spurs_pm_record.inc) hands each event of its runs to
 * wj_shadow_event, on the worker thread running it. From the bytes the run
 * observed (captured GETs, GETLLARs, successful PUTLLCs), the comparison asks
 * the core what the native manager would publish and compares that with what
 * the lifted run published:
 *
 *   claim      every job-list PUTLLC that advances N: claimable and the exact line
 *   retire     every job-list PUTLLC that keeps N: the exact line
 *   stop       the entry at N when a run exits or finish claims no job
 *   decrement  every dependency-counter PUTLLC: the exact line
 *   wake       the ready count a decrement to zero names: exact (spec 12.2)
 *   ready      the run's own ready-count writes: the rule, or a hint class
 *   plan       each job's command portion: accepted
 *   load       module copies present, resident copies not loaded again
 *   gather     every gathered piece present at its place
 *   context    the job API context words; the parameter data offset
 *   getbuf     every get-buffer result; release; finish results
 *
 * Allowed differences are counted by class, not as mismatches (spec 12): ring
 * placement (the recorded start is adopted, 12.1), and ready-count hints
 * (preclaim, gate-unread, zero after a job claim, busy after claiming an
 * uncounted entry, values 1-4; 12.2). A
 * difference no rule explains is a mismatch: counted, its first instances
 * printed. A summary line is printed every 5 s and at exit. */

enum { WS_RUN_BEGIN = 1, WS_RUN_END, WS_COMMAND, WS_TRANSFER, WS_ATOMIC, WS_BRANCH, WS_KERNEL };
enum { WS_ENTER = 1, WS_RETURN, WS_CALL, WS_SVC_RETURN };
enum { WS_POLICY = 0, WS_JOB = 1, WS_SERVICE = 2 };
#define WS_NO_SLOT 0xFFFFFFFFu

enum ws_class {
    WSC_CLAIM, WSC_RETIRE, WSC_STOP, WSC_DECREMENT, WSC_WAKE, WSC_READY, WSC_READY_FLAG, WSC_PLAN, WSC_LOAD,
    WSC_LOAD_SKIP, WSC_GATHER, WSC_CONTEXT, WSC_USERDATA, WSC_GETBUF, WSC_RELEASE, WSC_FINISH,
    /* allowed differences (spec 12) */
    WSC_PLACEMENT, WSC_READY_PRECLAIM, WSC_READY_GATE_UNREAD, WSC_READY_ZERO_AFTER_JOB, WSC_READY_UNCOUNTED, WSC_READY_U17,
    WSC_COUNT
};
static const char* const k_ws_names[WSC_COUNT] = {
    "claim", "retire", "stop", "decrement", "wake", "ready", "ready_flag", "plan", "load", "load_skip", "gather",
    "context", "userdata", "getbuf", "release", "finish",
    "placement", "ready_preclaim", "ready_gate_unread", "ready_zero_after_job_claim", "ready_after_uncounted_claim",
    "ready_1_4" };
static struct {
    unsigned long long ok[WSC_COUNT], bad[WSC_COUNT], unk[WSC_COUNT];
    unsigned long long runs, runs_identical, runs_with_hints, jobs, printed;
    uint64_t next_print_ns, first_ns;
} s_ws;
#define WS_ADD(f, n) __atomic_add_fetch(&s_ws.f, (unsigned long long)(n), __ATOMIC_RELAXED)

static uint64_t ws_now(void)
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

void spurs_wwsjm_shadow_print(void);
void spurs_wwsjm_shadow_print(void)
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[wwsjm-shadow] runs=%llu identical=%llu with_allowed_differences=%llu jobs=%llu",
                     __atomic_load_n(&s_ws.runs, __ATOMIC_RELAXED), __atomic_load_n(&s_ws.runs_identical, __ATOMIC_RELAXED),
                     __atomic_load_n(&s_ws.runs_with_hints, __ATOMIC_RELAXED), __atomic_load_n(&s_ws.jobs, __ATOMIC_RELAXED));
    unsigned long long mism = 0, unk = 0;
    for (int part = 0; part < 3; part++) {
        n += snprintf(buf + n, sizeof buf - (size_t)n, part == 0 ? " ok{" : part == 1 ? "} mismatch{" : "} unverifiable{");
        for (int c = 0; c < WSC_COUNT && n < (int)sizeof buf - 64; c++) {
            const unsigned long long v = __atomic_load_n(part == 0 ? &s_ws.ok[c] : part == 1 ? &s_ws.bad[c] : &s_ws.unk[c],
                                                         __ATOMIC_RELAXED);
            if (part == 1) mism += v;
            if (part == 2) unk += v;
            if (v) n += snprintf(buf + n, sizeof buf - (size_t)n, " %s=%llu", k_ws_names[c], v);
        }
    }
    snprintf(buf + n, sizeof buf - (size_t)n, "} mismatches=%llu unverifiable=%llu\n", mism, unk);
    fputs(buf, stderr);
    fflush(stderr);
}

/* ---- observed guest bytes: a direct-mapped cache of lines with byte validity -- */
enum { WS_MEM_LINES = 16384, WS_MAX_XFER = 8192, WS_MAX_HELD = 8, WS_MAX_KEYS = 32, WS_MAX_READS = 64, WS_MAX_WAKE = 8 };
struct ws_mline { uint32_t ea, stamp; uint8_t used; uint8_t valid[16]; uint8_t b[128]; };

struct ws_key { int used; unsigned array; uint16_t index; uint8_t raw[8]; wwsjm_header h; uint32_t claim_seq;
                int under_job; };
struct ws_job {
    int valid, native, retired, unreserved, has_plan;
    unsigned array; uint16_t index;
    wwsjm_plan plan;
    uint32_t cmd_ls, deps_left[WWSJM_MAX_DEPS];
    unsigned deps_n, tags;
    struct { uint8_t set, buf, tag; } tag_of[WWSJM_JOB_TAG_COUNT]; unsigned tag_n;
};
struct ws_xfer { uint32_t seq, lsa, ea, size; uint8_t region, payload, get; };

struct ws_thread {
    struct ws_mline mem[WS_MEM_LINES];     /* 4-way sets, hashed by line address */
    uint32_t stamp;
    /* the current run */
    int active;
    uint32_t wid, lane, list_ea, spurs_ea, r5, k_inst, busy, run;
    uint32_t seq;
    struct { uint32_t ea; uint8_t line[128]; int valid; } ll[8];
    uint8_t list_view[128]; int has_view;
    struct { unsigned array; uint16_t index; } held[WS_MAX_HELD]; unsigned held_n;
    struct ws_key keys[WS_MAX_KEYS];
    struct ws_job job, returned;
    struct { unsigned wid; uint8_t value; } wake[WS_MAX_WAKE]; unsigned wake_n;
    int in_finish, finish_claim;          /* key index or -1 */
    int instance_writes, last_claim_type;
    uint32_t last_poll_status;
    int poll_other;                       /* the last select answered another workload */
    int flag_owed;                        /* a mid-run poll reported the flag: ready := 8 owed */
    int pre_valid; uint16_t pre_S, pre_N;
    struct { uint32_t ea, size; } reads[WS_MAX_READS]; unsigned reads_n;
    struct ws_xfer* x; unsigned x_n;
    wwsjm_residency res;
    uint32_t last_call_r4, last_call_r5, last_call_r6, last_call_slot;
    unsigned bad, unk, hint;
};
static SPU_THREAD_LOCAL struct ws_thread* t_ws;

static uint32_t ws_u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t ws_u64(const uint8_t* p) { return (uint64_t)ws_u32(p) | (uint64_t)ws_u32(p + 4) << 32; }

static void ws_ok(struct ws_thread* t, enum ws_class c)
{
    WS_ADD(ok[c], 1);
    if (c >= WSC_PLACEMENT) t->hint++;
}
static void ws_bad(struct ws_thread* t, enum ws_class c, const char* fmt, uint32_t a, uint32_t b, uint32_t d)
{
    WS_ADD(bad[c], 1);
    t->bad++;
    if (__atomic_add_fetch(&s_ws.printed, 1, __ATOMIC_RELAXED) <= 40) {
        fprintf(stderr, "[wwsjm-shadow] MISMATCH %s wid=%u lane=%u run=%u ev=%u: ", k_ws_names[c], t->wid, t->lane, t->run, t->seq);
        fprintf(stderr, fmt, a, b, d);
        fprintf(stderr, "\n");
    }
}
static void ws_unk_at(struct ws_thread* t, enum ws_class c, int line)
{
    WS_ADD(unk[c], 1);
    t->unk++;
    static int s_dbg = -1;
    if (s_dbg < 0) s_dbg = getenv("PS3RECOMP_WWSJM_SHADOW_DEBUG") ? 1 : 0;
    if (s_dbg) fprintf(stderr, "[wwsjm-shadow] unverifiable %s wid=%u lane=%u seq=%u (wwsjm_shadow.inc:%d)\n",
                       k_ws_names[c], t->wid, t->lane, t->seq, line);
}
#define ws_unk(t, c) ws_unk_at((t), (c), __LINE__)

static struct ws_mline* ws_mem_find(struct ws_thread* t, uint32_t line, int make)
{
    struct ws_mline* set = &t->mem[((line >> 7) * 0x9E3779B1u >> 11) % (WS_MEM_LINES / 4) * 4];
    struct ws_mline* victim = set;
    for (unsigned w = 0; w < 4; w++) {
        if (set[w].used && set[w].ea == line) { set[w].stamp = ++t->stamp; return &set[w]; }
        if (!set[w].used || set[w].stamp < victim->stamp) victim = &set[w];
    }
    if (!make) return NULL;
    victim->used = 1; victim->ea = line; victim->stamp = ++t->stamp; memset(victim->valid, 0, 16);
    return victim;
}

static void ws_mem_write(struct ws_thread* t, uint32_t ea, const uint8_t* p, uint32_t n)
{
    while (n) {
        const uint32_t line = ea & ~127u, o = ea & 127u, k = n < 128 - o ? n : 128 - o;
        struct ws_mline* m = ws_mem_find(t, line, 1);
        memcpy(m->b + o, p, k);
        for (uint32_t i = o; i < o + k; i++) m->valid[i >> 3] |= (uint8_t)(1u << (i & 7));
        ea += k; p += k; n -= k;
    }
}
static int ws_mem_read(struct ws_thread* t, uint32_t ea, uint32_t n, uint8_t* out)
{
    while (n) {
        const uint32_t line = ea & ~127u, o = ea & 127u, k = n < 128 - o ? n : 128 - o;
        const struct ws_mline* m = ws_mem_find(t, line, 0);
        if (!m) return 0;
        for (uint32_t i = o; i < o + k; i++) if (!(m->valid[i >> 3] >> (i & 7) & 1)) return 0;
        memcpy(out, m->b + o, k);
        ea += k; out += k; n -= k;
    }
    return 1;
}

static uint8_t* ws_ll(struct ws_thread* t, uint32_t ea)
{
    for (unsigned i = 0; i < 8; i++) if (t->ll[i].valid && t->ll[i].ea == ea) return t->ll[i].line;
    return NULL;
}
static void ws_ll_set(struct ws_thread* t, uint32_t ea, const uint8_t* line)
{
    unsigned slot = 0;
    for (unsigned i = 0; i < 8; i++) { if (t->ll[i].valid && t->ll[i].ea == ea) { slot = i; break; } if (!t->ll[i].valid) slot = i; }
    t->ll[slot].valid = 1; t->ll[slot].ea = ea; memcpy(t->ll[slot].line, line, 128);
}

/* The core's view of the entry at N of `line`'s current array, from observed
 * bytes: 1 with verdict, 0 when the needed bytes were not observed. */
static int ws_entry(struct ws_thread* t, const uint8_t* line, wwsjm_cursor* cur, wwsjm_header* h, uint8_t raw[8],
                    wwsjm_verdict* v)
{
    wwsjm_line dl;
    wwsjm_line_decode(line, &dl);
    if (wwsjm_cursor_of(&dl, cur) != WWSJM_OK) return 0;
    if (!ws_mem_read(t, cur->header_ea, 8, raw)) return 0;
    if (wwsjm_header_decode(raw, h) != WWSJM_OK) return 0;
    uint16_t gate = 0;
    if (wwsjm_header_is_gate(h)) {
        uint8_t g[2];
        if (!ws_mem_read(t, h->ea, 2, g)) return 0;
        gate = wwsjm_be16(g);
    }
    *v = wwsjm_judge(h, gate);
    return 1;
}

static int ws_find_key(struct ws_thread* t, unsigned array, uint16_t index)
{
    for (int i = 0; i < WS_MAX_KEYS; i++)
        if (t->keys[i].used && t->keys[i].array == array && t->keys[i].index == index) return i;
    return -1;
}
static int ws_new_key(struct ws_thread* t, unsigned array, uint16_t index)
{
    int i = ws_find_key(t, array, index);
    if (i < 0) for (i = 0; i < WS_MAX_KEYS && t->keys[i].used; i++) {}
    if (i >= WS_MAX_KEYS) i = (int)(t->seq % WS_MAX_KEYS);   /* oldest unknown: reuse */
    memset(&t->keys[i], 0, sizeof t->keys[i]);
    t->keys[i].used = 1; t->keys[i].array = array; t->keys[i].index = index;
    return i;
}
static int ws_payload_active(const struct ws_thread* t)
{
    return (t->job.valid && t->job.native) ||
           (t->returned.valid && t->returned.native && !t->returned.retired);
}

/* ---- line transactions ---------------------------------------------------------------- */
static void ws_list_tx(struct ws_thread* t, const uint8_t* old, const uint8_t* now, uint32_t status)
{
    const unsigned a = wwsjm_be16(old + 0x0A);
    if (a > 3) { ws_bad(t, WSC_CLAIM, "current array %u", a, 0, 0); return; }
    if (wwsjm_be16(old + 0x42 + 16 * a) != wwsjm_be16(now + 0x42 + 16 * a)) {
        wwsjm_cursor cur; wwsjm_header h; uint8_t raw[8]; wwsjm_verdict v;
        if (!ws_entry(t, old, &cur, &h, raw, &v)) { ws_unk(t, WSC_CLAIM); return; }
        if (v != WWSJM_TAKE) { ws_bad(t, WSC_CLAIM, "claimed an entry the core would not (verdict %u, header %08X %08X)",
                                      v, wwsjm_be32(raw), wwsjm_be32(raw + 4)); return; }
        uint8_t want[128];
        if (wwsjm_claim(old, t->lane, &h, want) != WWSJM_OK || memcmp(want, now, 128) != 0) {
            unsigned o = 0; while (o < 128 && want[o] == now[o]) o++;
            ws_bad(t, WSC_CLAIM, "claim line differs at +0x%02X (type %u index 0x%04X)", o, h.type, cur.index);
            return;
        }
        ws_ok(t, WSC_CLAIM);
        if (status != 0) return;
        wwsjm_line dl; wwsjm_line_decode(old, &dl);
        t->pre_valid = 1; t->pre_S = dl.m[a].submitted; t->pre_N = dl.m[a].next;
        t->last_claim_type = h.type;
        if (h.type == WWSJM_ENTRY_JOB) {
            const int k = ws_new_key(t, cur.array, cur.index);
            memcpy(t->keys[k].raw, raw, 8); t->keys[k].h = h; t->keys[k].claim_seq = t->seq;
            if (t->held_n < WS_MAX_HELD) { t->held[t->held_n].array = cur.array; t->held[t->held_n].index = cur.index; t->held_n++; }
            if (t->in_finish) t->finish_claim = k;
        }
        return;
    }
    /* retire */
    if (!t->held_n) { ws_unk(t, WSC_RETIRE); return; }
    wwsjm_held next = { t->held_n > 1, t->held_n > 1 ? t->held[1].array : 0, t->held_n > 1 ? t->held[1].index : 0 };
    uint8_t want[128];
    if (wwsjm_retire(old, t->held[0].array, t->lane, &next, want) != WWSJM_OK || memcmp(want, now, 128) != 0) {
        ws_bad(t, WSC_RETIRE, "retire of %u:0x%04X differs (next %u)", t->held[0].array, t->held[0].index, next.valid);
        return;
    }
    ws_ok(t, WSC_RETIRE);
    if (status != 0) return;
    const int k = ws_find_key(t, t->held[0].array, t->held[0].index);
    if (k >= 0) t->keys[k].used = 0;          /* (array, index) repeats once a list is re-armed */
    memmove(&t->held[0], &t->held[1], (t->held_n - 1) * sizeof t->held[0]);
    t->held_n--;
    if (t->returned.valid) t->returned.retired = 1;
}

static void ws_ready_tx(struct ws_thread* t, const uint8_t* old, const uint8_t* now, uint32_t status)
{
    const int first = t->instance_writes == 0;
    if (status == 0) t->instance_writes++;
    uint8_t want[128];
    if (t->r5 == 4 && first) {
        wwsjm_ready_apply(old, t->wid, WWSJM_READY_ON_FLAG, want);
        if (!memcmp(want, now, 128)) ws_ok(t, WSC_READY_FLAG);
        else ws_bad(t, WSC_READY_FLAG, "flag entry wrote %u (expected 8)", now[t->wid], 0, 0);
        if (status != 0) t->instance_writes = 0;
        return;
    }
    if (t->flag_owed) {   /* a mid-run poll reported the flag: ready := 8 (6.1, U19) */
        wwsjm_ready_apply(old, t->wid, WWSJM_READY_ON_FLAG, want);
        if (!memcmp(want, now, 128)) { ws_ok(t, WSC_READY_FLAG); if (status == 0) t->flag_owed = 0; return; }
    }
    if (t->wake_n) {
        if (wwsjm_ready_apply(old, t->wake[0].wid, t->wake[0].value, want) == WWSJM_OK && !memcmp(want, now, 128)) {
            ws_ok(t, WSC_WAKE);
            if (status == 0) { memmove(&t->wake[0], &t->wake[1], (t->wake_n - 1) * sizeof t->wake[0]); t->wake_n--; }
            return;
        }
    }
    for (unsigned o = 0; o < 128; o++)
        if (old[o] != now[o] && o != t->wid) { ws_bad(t, WSC_READY, "instance byte 0x%02X written (wid %u)", o, t->wid, 0); return; }
    if (!t->has_view) { ws_unk(t, WSC_READY); return; }
    wwsjm_cursor cur; wwsjm_header h; uint8_t raw[8]; wwsjm_verdict v;
    if (!ws_entry(t, t->list_view, &cur, &h, raw, &v)) { ws_unk(t, WSC_READY); return; }
    wwsjm_line dl; wwsjm_line_decode(t->list_view, &dl);
    const uint8_t got = now[t->wid], want_v = wwsjm_ready_value(&dl, v, (uint8_t)t->busy);
    if (got == want_v) { ws_ok(t, WSC_READY); return; }
    if (t->pre_valid && (got == 0 || got == t->busy) &&
        got == ((int16_t)(uint16_t)(t->pre_S - t->pre_N) > 0 ? t->busy : 0)) { ws_ok(t, WSC_READY_PRECLAIM); return; }
    if (got == 0 && want_v == t->busy && wwsjm_header_is_gate(&h)) {
        int read = 0;
        for (unsigned i = 0; i < t->reads_n; i++)
            if (t->reads[i].ea <= h.ea && h.ea + 2 <= t->reads[i].ea + t->reads[i].size) read = 1;
        if (!read) { ws_ok(t, WSC_READY_GATE_UNREAD); return; }
    }
    if (got == 0 && want_v == t->busy && t->last_claim_type == WWSJM_ENTRY_JOB) { ws_ok(t, WSC_READY_ZERO_AFTER_JOB); return; }
    /* busy right after claiming an entry the publisher had not counted (the
     * claim itself raised S): the original advertises work it just took */
    if (got == t->busy && want_v == 0 && t->pre_valid && t->pre_S == t->pre_N) { ws_ok(t, WSC_READY_UNCOUNTED); return; }
    if (got >= 1 && got <= 4) { ws_ok(t, WSC_READY_U17); return; }
    ws_bad(t, WSC_READY, "ready wrote %u, rule says %u (verdict and last poll status 0x%X)", got, want_v,
           (uint32_t)v | t->last_poll_status << 8);
}

static void ws_dep_tx(struct ws_thread* t, uint32_t ea, const uint8_t* old, const uint8_t* now, uint32_t status)
{
    struct ws_job* j = &t->returned;
    if (!j->valid || !j->has_plan) { ws_unk(t, WSC_DECREMENT); return; }
    int at = -1;
    for (unsigned i = 0; i < j->deps_n; i++) if ((j->deps_left[i] & ~127u) == ea) { at = (int)i; break; }
    if (at < 0) { ws_bad(t, WSC_DECREMENT, "PUTLLC at 0x%08X names no dependency of the job", ea, 0, 0); return; }
    const uint32_t off = j->deps_left[at] & 127u;
    uint8_t want[128]; uint16_t v;
    if (wwsjm_decrement(old, off, want, &v) != WWSJM_OK || memcmp(want, now, 128) != 0) {
        ws_bad(t, WSC_DECREMENT, "counter at 0x%08X+0x%02X differs", ea, off, 0); return;
    }
    ws_ok(t, WSC_DECREMENT);
    if (status != 0) return;
    j->deps_left[at] = j->deps_left[--j->deps_n];
    if (v) return;
    uint8_t f[2];
    if (off + 2 < 128) memcpy(f, now + off + 2, 2);
    else if (!ws_mem_read(t, ea + 128, 2, f)) { ws_unk(t, WSC_WAKE); return; }
    if (t->wake_n < WS_MAX_WAKE) {
        unsigned w; uint8_t val;
        wwsjm_wake_target(wwsjm_be16(f), &w, &val);
        t->wake[t->wake_n].wid = w; t->wake[t->wake_n].value = val; t->wake_n++;
    }
}

static void ws_atomic(struct ws_thread* t, const uint8_t* p, uint32_t n)
{
    if (n < 24 + 128) return;
    const uint32_t cmd = ws_u32(p), ea = ws_u32(p + 8), status = ws_u32(p + 12), region = ws_u32(p + 16);
    const uint8_t* line = p + 24;
    if (cmd == MFC_GETLLAR_CMD) {
        ws_ll_set(t, ea, line);
        ws_mem_write(t, ea, line, 128);
        /* the manager's view of the list is what the manager read: a job's own
         * GETLLAR of the line (into its reservation buffer) is not one */
        if (ea == t->list_ea && region != WS_JOB) {
            memcpy(t->list_view, line, 128); t->has_view = 1; t->pre_valid = 0; t->reads_n = 0;
        }
        return;
    }
    if (cmd != MFC_PUTLLC_CMD) return;
    if (status == 0) {
        ws_mem_write(t, ea, line, 128);
        if (ea == t->list_ea) { memcpy(t->list_view, line, 128); t->has_view = 1; }
    }
    if (region == WS_JOB) return;
    if (ws_payload_active(t) && ea != t->list_ea && ea != t->spurs_ea) return;   /* the payload's own */
    const uint8_t* old = ws_ll(t, ea);
    if (!old) { ws_unk(t, ea == t->list_ea ? WSC_CLAIM : ea == t->spurs_ea ? WSC_READY : WSC_DECREMENT); return; }
    uint8_t o[128]; memcpy(o, old, 128);
    if (ea == t->list_ea) ws_list_tx(t, o, line, status);
    else if (ea == t->spurs_ea) ws_ready_tx(t, o, line, status);
    else ws_dep_tx(t, ea, o, line, status);
}

static void ws_stop_check(struct ws_thread* t)
{
    /* the kernel asked for another workload: the stop is the kernel's, whatever
     * the line (read later, e.g. by the ready rule) shows */
    if (t->poll_other) { ws_ok(t, WSC_STOP); return; }
    if (!t->has_view) { ws_unk(t, WSC_STOP); return; }
    wwsjm_cursor cur; wwsjm_header h; uint8_t raw[8]; wwsjm_verdict v;
    if (!ws_entry(t, t->list_view, &cur, &h, raw, &v)) { ws_unk(t, WSC_STOP); return; }
    if (v == WWSJM_TAKE) { ws_bad(t, WSC_STOP, "stopped before a claimable entry (index 0x%04X type %u)", cur.index, h.type, 0); return; }
    ws_ok(t, WSC_STOP);
}

/* ---- jobs ------------------------------------------------------------------------------- */
static int ws_window_has(const struct ws_thread* t, uint32_t from, uint32_t lsa, uint32_t ea, uint32_t size)
{
    const unsigned held = t->x_n < WS_MAX_XFER ? t->x_n : WS_MAX_XFER;
    for (unsigned i = 0; i < held; i++) {
        const struct ws_xfer* x = &t->x[(t->x_n - 1 - i) % WS_MAX_XFER];
        if (x->seq < from) break;
        if (x->get && x->lsa == lsa && x->ea == ea && x->size == size) return 1;
    }
    return 0;
}

/* Spec 18.1: an element may run into the list copy only over entries already
 * consumed: element k ends at or before the copy's start + 8*(k+1). */
static int ws_gather_consumed_ok(const uint8_t* list, const wwsjm_load* l, const wwsjm_piece* pc, unsigned n)
{
    const uint32_t copy = l->ls + l->limit - l->bytes;
    unsigned piece = 0;
    for (unsigned k = 0; k < l->bytes / 8 && piece < n; k++) {
        if (!wwsjm_be32(list + 8u * k)) continue;
        const uint32_t end = pc[piece].ls + pc[piece].bytes;
        if (end > copy && end > copy + 8u * (k + 1)) return 0;
        piece++;
    }
    return 1;
}

/* Try other ring starts for set q until `test` passes; counted as placement. */
static int ws_try_starts(struct ws_thread* t, unsigned q, uint32_t from)
{
    wwsjm_plan* p = &t->job.plan;
    const uint8_t orig = p->set[q].start;
    for (unsigned s = 0; s < p->set[q].count; s++) {
        if (s == orig) continue;
        p->set[q].start = (uint8_t)s;
        wwsjm_plan_relocate(p);
        int all = 1;
        for (unsigned i = 0; i < p->load_count && all; i++) {
            const wwsjm_load* l = &p->load[i];
            if (p->use[l->use].set != q || l->fill != WWSJM_FILL_GATHER) continue;
            uint8_t list[1024];
            if (l->bytes > sizeof list || !ws_mem_read(t, l->ea, l->bytes, list)) { all = 0; break; }
            wwsjm_piece pc[128]; unsigned n = 0;
            if (wwsjm_gather_layout(list, l->bytes / 8, l->ls, l->limit, pc, 128, &n) != WWSJM_OK) { all = 0; break; }
            for (unsigned k = 0; k < n && all; k++) all = ws_window_has(t, from, pc[k].ls, pc[k].ea, pc[k].bytes);
        }
        if (all) { ws_ok(t, WSC_PLACEMENT); return 1; }
    }
    p->set[q].start = orig;
    wwsjm_plan_relocate(p);
    return 0;
}

/* Effects the previous job still owed when the next job starts or the run ends. */
static void ws_owed(struct ws_thread* t)
{
    if (t->returned.valid && t->returned.has_plan && t->returned.deps_n) {
        ws_bad(t, WSC_DECREMENT, "job %u:0x%04X ended with %u dependency counters not decremented",
               t->returned.array, t->returned.index, t->returned.deps_n);
        t->returned.deps_n = 0;
    }
    if (t->wake_n) {
        ws_bad(t, WSC_WAKE, "a counter reached zero but workload %u was not given ready count %u (%u pending)",
               t->wake[0].wid, t->wake[0].value, t->wake_n);
        t->wake_n = 0;
    }
    if (t->flag_owed) {   /* a poll reported the flag, and no ready := 8 followed (6.1, U19) */
        ws_bad(t, WSC_READY_FLAG, "a poll reported the flag but ready := 8 was not written", 0, 0, 0);
        t->flag_owed = 0;
    }
}

static void ws_enter(struct ws_thread* t, unsigned array, uint16_t index, int native, const uint8_t* ctxb, uint32_t ctx_n)
{
    ws_owed(t);
    struct ws_job* j = &t->job;
    memset(j, 0, sizeof *j);
    j->valid = 1; j->native = native; j->array = array; j->index = index;
    WS_ADD(jobs, 1);
    const int k = ws_find_key(t, array, index);
    if (k < 0) { ws_bad(t, WSC_CLAIM, "job %u:0x%04X entered without a successful claim of it", array, index, 0); return; }
    const struct ws_key* key = &t->keys[k];
    uint8_t rec[WWSJM_MAX_PORTION];
    j->cmd_ls = 0xFFFFFFFFu;
    for (unsigned i = 0; i < (t->x_n < WS_MAX_XFER ? t->x_n : WS_MAX_XFER); i++) {
        const struct ws_xfer* x = &t->x[(t->x_n - 1 - i) % WS_MAX_XFER];
        if (x->seq < key->claim_seq) break;
        if (x->get && x->ea == key->h.ea && x->size == key->h.size) { j->cmd_ls = x->lsa; break; }
    }
    if (!ws_mem_read(t, key->h.ea, key->h.size, rec)) { ws_unk(t, WSC_PLAN); return; }
    unsigned bad = 0;
    const wwsjm_status st = wwsjm_plan_commands(rec, key->h.size, 0x4000, WWSJM_LS_BYTES, &j->plan, &bad);
    if (st != WWSJM_OK) { ws_bad(t, WSC_PLAN, "record refused: %u at command %u (word 0x%08X)", st, bad, wwsjm_be32(rec + 8 * (bad < 32 ? bad : 0))); return; }
    ws_ok(t, WSC_PLAN);
    j->has_plan = 1;
    j->deps_n = j->plan.dep_count;
    for (unsigned i = 0; i < j->deps_n; i++) j->deps_left[i] = j->plan.dep_ea[i];
    wwsjm_plan_place(&j->plan, NULL, 0);
    wwsjm_plan_residency(&j->plan, &t->res);
    for (unsigned i = 0; i < j->plan.load_count; i++) {
        const wwsjm_load* l = &j->plan.load[i];
        if (l->fill == WWSJM_FILL_COPY) {
            const int seen = ws_window_has(t, key->claim_seq, l->ls, l->ea, l->bytes);
            if (l->skip) { if (seen) ws_bad(t, WSC_LOAD, "resident copy reloaded 0x%05X 0x%08X 0x%X", l->ls, l->ea, l->bytes); else ws_ok(t, WSC_LOAD_SKIP); }
            else if (seen) ws_ok(t, WSC_LOAD);
            else if (!(j->plan.set[j->plan.use[l->use].set].count > 1 && ws_try_starts(t, j->plan.use[l->use].set, key->claim_seq)))
                ws_bad(t, WSC_LOAD, "copy not loaded 0x%05X 0x%08X 0x%X", l->ls, l->ea, l->bytes);
            continue;
        }
        uint8_t list[1024];
        if (l->bytes > sizeof list || !ws_mem_read(t, l->ea, l->bytes, list)) { ws_unk(t, WSC_GATHER); continue; }
        wwsjm_piece pc[128]; unsigned n = 0;
        if (wwsjm_gather_layout(list, l->bytes / 8, l->ls, l->limit, pc, 128, &n) != WWSJM_OK ||
            !ws_gather_consumed_ok(list, l, pc, n)) {
            ws_bad(t, WSC_GATHER, "gather list refused at 0x%08X (%u bytes)", l->ea, l->bytes, 0); continue;
        }
        int all = 1;
        for (unsigned q = 0; q < n && all; q++) all = ws_window_has(t, key->claim_seq, pc[q].ls, pc[q].ea, pc[q].bytes);
        if (!all) {
            const unsigned set = j->plan.use[l->use].set;
            if (!(j->plan.set[set].count > 1 && ws_try_starts(t, set, key->claim_seq))) {
                ws_bad(t, WSC_GATHER, "gathered pieces of the list at 0x%08X not at their places (set %u)", l->ea, set, 0);
                continue;
            }
        }
        /* the list copy in the buffer's last 16*L bytes, where the job reads it (17.1) */
        if (!ws_window_has(t, key->claim_seq, l->ls + l->limit - l->bytes, l->ea, l->bytes)) {
            ws_bad(t, WSC_GATHER, "gather list at 0x%08X not copied to its buffer's end (LS 0x%05X)", l->ea,
                   l->ls + l->limit - l->bytes, 0);
            continue;
        }
        WS_ADD(ok[WSC_GATHER], n);
    }
    if (native || !ctxb || ctx_n < 0x30) return;
    uint8_t want[WWSJM_CONTEXT_BYTES];
    wwsjm_context_args a;
    memset(&a, 0, sizeof a);
    a.lane = t->lane; a.dma_tag = 8; a.spurs_ea = t->spurs_ea; a.wid = t->wid; a.index = index; a.array = array;
    a.dep_count = j->plan.dep_count; a.list_ea = t->list_ea; a.list_tag = t->k_inst;
    wwsjm_context_build(&a, want);
    if (memcmp(ctxb, want, 0x20) != 0) {
        unsigned o = 0; while (o < 0x20 && ctxb[o] == want[o]) o++;
        ws_bad(t, WSC_CONTEXT, "context word +0x%02X: 0x%08X, expected 0x%08X", o & ~3u, wwsjm_be32(ctxb + (o & ~3u)), wwsjm_be32(want + (o & ~3u)));
    } else ws_ok(t, WSC_CONTEXT);
    const uint32_t ud = wwsjm_be32(ctxb + 0x20);
    if (j->cmd_ls == 0xFFFFFFFFu) ws_unk(t, WSC_USERDATA);
    else if (ud - j->cmd_ls != j->plan.userdata_offset || wwsjm_be32(ctxb + 0x24) != ud || wwsjm_be32(ctxb + 0x2C) != ud)
        ws_bad(t, WSC_USERDATA, "parameter data at +0x%X of the command copy, expected +0x%X", ud - j->cmd_ls, j->plan.userdata_offset, 0);
    else ws_ok(t, WSC_USERDATA);
}

static void ws_branch(struct ws_thread* t, const uint8_t* p, uint32_t n)
{
    if (n < 68) return;
    const uint32_t kind = ws_u32(p), slot = ws_u32(p + 48), extra_n = ws_u32(p + 64);
    const uint8_t* extra = p + 68;
    if (extra_n > n - 68) return;
    if (kind == WS_ENTER) {
        if (extra_n < 0x30) { ws_unk(t, WSC_PLAN); return; }
        const unsigned array = wwsjm_be32(extra + 0x18) & 3u;
        const uint16_t index = (uint16_t)wwsjm_be32(extra + 0x10);
        if (t->job.valid && t->job.native && t->job.array == array && t->job.index == index) {
            t->job.native = 0;   /* the payload override declined: the lifted job runs with the same staging */
            return;
        }
        ws_enter(t, array, index, 0, extra, extra_n);
        return;
    }
    if (kind == WS_RETURN) {
        if (t->job.valid) { t->returned = t->job; t->job.valid = 0; }
        return;
    }
    if (kind == WS_CALL) {
        if (!t->job.valid) {
            /* a job taken over by a payload override: no entry event, its first service call */
            for (unsigned i = 0; i < t->held_n; i++) {
                const int k = ws_find_key(t, t->held[i].array, t->held[i].index);
                if (k >= 0 && !(t->returned.valid && t->returned.array == t->held[i].array && t->returned.index == t->held[i].index)) {
                    ws_enter(t, t->held[i].array, t->held[i].index, 1, NULL, 0);
                    break;
                }
            }
        }
        if (slot == 0x44) { t->in_finish = 1; t->finish_claim = -1; }
        t->last_call_r4 = ws_u32(p + 32); t->last_call_r5 = ws_u32(p + 36); t->last_call_r6 = ws_u32(p + 40);
        t->last_call_slot = slot;
        return;
    }
    if (kind != WS_SVC_RETURN) return;
    const uint32_t r3[4] = { ws_u32(p + 16), ws_u32(p + 20), ws_u32(p + 24), ws_u32(p + 28) };
    if (slot == 0x44) {
        t->in_finish = 0;
        uint32_t want[2];
        if (t->finish_claim < 0) { wwsjm_finish_result(NULL, want); ws_stop_check(t); }
        else wwsjm_finish_result(&t->keys[t->finish_claim].h, want);
        if (r3[0] == want[0] && r3[1] == want[1]) ws_ok(t, WSC_FINISH);
        else ws_bad(t, WSC_FINISH, "finish returned %08X %08X, expected %08X", r3[0], r3[1], want[0]);
        if (t->job.valid && t->job.native) { t->returned = t->job; t->job.valid = 0; }
        return;
    }
    if (slot == 0x48) {
        if (r3[0] == 0xFFFFFFFFu) ws_ok(t, WSC_RELEASE); else ws_bad(t, WSC_RELEASE, "release returned 0x%08X", r3[0], 0, 0);
        return;
    }
    if (slot != 0x34) return;
    if (!t->job.valid || !t->job.has_plan) { ws_unk(t, WSC_GETBUF); return; }
    uint8_t tag = 0;
    if (t->last_call_r6) {   /* one tag per (set, buffer) per job, memoised (5.3, 18.2) */
        struct ws_job* j = &t->job;
        unsigned i = 0;
        while (i < j->tag_n && (j->tag_of[i].set != t->last_call_r4 || j->tag_of[i].buf != t->last_call_r5)) i++;
        if (i == j->tag_n) {
            if (i >= WWSJM_JOB_TAG_COUNT) {   /* the run loop stops the process here */
                ws_bad(t, WSC_GETBUF, "more DMA tags than a job may take (set %u buffer %u)", t->last_call_r4, t->last_call_r5, 0);
                return;
            }
            j->tag_of[i].set = (uint8_t)t->last_call_r4; j->tag_of[i].buf = (uint8_t)t->last_call_r5;
            j->tag_of[i].tag = (uint8_t)(31u - j->tags++); j->tag_n++;
        }
        tag = j->tag_of[i].tag;
    }
    uint8_t d[16];
    wwsjm_status st = wwsjm_buffer_descriptor(&t->job.plan, t->last_call_r4, t->last_call_r5, tag, d);
    uint8_t got[16];
    for (unsigned i = 0; i < 4; i++) wwsjm_put32(got + 4 * i, r3[i]);
    if (st == WWSJM_OK && memcmp(d, got, 16) != 0 && t->last_call_r4 < 16 && t->job.plan.set[t->last_call_r4].count > 1) {
        wwsjm_set* s = &t->job.plan.set[t->last_call_r4];
        const uint8_t orig = s->start;
        for (unsigned v = 0; v < s->count; v++) {
            s->start = (uint8_t)v;
            wwsjm_buffer_descriptor(&t->job.plan, t->last_call_r4, t->last_call_r5, tag, d);
            if (!memcmp(d, got, 16)) { wwsjm_plan_relocate(&t->job.plan); if (v != orig) ws_ok(t, WSC_PLACEMENT); break; }
            s->start = orig;
        }
    }
    if (st != WWSJM_OK) ws_bad(t, WSC_GETBUF, "get-buffer of set %u buffer %u refused by the core (%u)", t->last_call_r4, t->last_call_r5, st);
    else if (memcmp(d, got, 16) != 0) {
        ws_bad(t, WSC_GETBUF, "get-buffer of set %u buffer %u: word 1 0x%08X differs", t->last_call_r4, t->last_call_r5, r3[1]);
        fprintf(stderr, "[wwsjm-shadow]   getbuf words recorded %08X %08X %08X %08X native %08X %08X %08X %08X"
                " (set: first page %u, pages %u, count %u, start %u)\n", r3[0], r3[1], r3[2], r3[3],
                wwsjm_be32(d), wwsjm_be32(d + 4), wwsjm_be32(d + 8), wwsjm_be32(d + 12),
                t->last_call_r4 < WWSJM_MAX_SETS ? t->job.plan.set[t->last_call_r4].first_page : 0,
                t->last_call_r4 < WWSJM_MAX_SETS ? t->job.plan.set[t->last_call_r4].pages : 0,
                t->last_call_r4 < WWSJM_MAX_SETS ? t->job.plan.set[t->last_call_r4].count : 0,
                t->last_call_r4 < WWSJM_MAX_SETS ? t->job.plan.set[t->last_call_r4].start : 0);
    }
    else ws_ok(t, WSC_GETBUF);
}

static struct ws_thread* ws_thread_state(void)
{
    struct ws_thread* t = t_ws;
    if (t) return t;
    t = (struct ws_thread*)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->x = (struct ws_xfer*)calloc(WS_MAX_XFER, sizeof *t->x);
    if (!t->x) { free(t); return NULL; }
    t_ws = t;
    return t;
}

static void wj_shadow_event(spu_context* c, uint32_t kind, const uint8_t* p, uint32_t n)
{
    (void)c;
    struct ws_thread* t = ws_thread_state();
    if (!t) return;
    if (kind == WS_RUN_BEGIN) {
        if (n < 64 + 128) return;
        t->active = 1;
        t->run = ws_u32(p); t->wid = ws_u32(p + 4); t->lane = ws_u32(p + 8); t->spurs_ea = ws_u32(p + 12);
        const uint64_t data = ws_u64(p + 16);
        t->list_ea = (uint32_t)data; t->r5 = ws_u32(p + 24);
        t->busy = 0;
        AcquireSRWLockShared(&s_wj_lock);
        for (unsigned i = 0; i < sizeof s_wj_inst / sizeof s_wj_inst[0]; i++)
            if (s_wj_inst[i].spurs_ea == t->spurs_ea) t->busy = s_wj_inst[i].n_spus;
        ReleaseSRWLockShared(&s_wj_lock);
        t->k_inst = 0x0A000000u;
        t->seq = 0; t->x_n = 0; t->held_n = 0; t->wake_n = 0; t->reads_n = 0;
        memset(t->ll, 0, sizeof t->ll); memset(t->keys, 0, sizeof t->keys);
        memset(&t->job, 0, sizeof t->job); memset(&t->returned, 0, sizeof t->returned);
        t->in_finish = 0; t->finish_claim = -1; t->instance_writes = 0; t->last_claim_type = -1; t->pre_valid = 0;
        t->last_poll_status = 0; t->poll_other = 0; t->flag_owed = 0;
        t->bad = t->unk = t->hint = 0;
        t->has_view = ws_u32(p + 52) != 0;
        if (t->has_view) memcpy(t->list_view, p + 64, 128);
        wwsjm_residency_reset(&t->res);
        WS_ADD(runs, 1);
        if (!s_ws.first_ns) s_ws.first_ns = ws_now();
        return;
    }
    if (!t->active) return;
    t->seq++;
    switch (kind) {
    case WS_TRANSFER: {
        if (n < 44) return;
        const uint32_t xfer = ws_u32(p), lsa = ws_u32(p + 8), size = ws_u32(p + 12), ea = (uint32_t)ws_u64(p + 16);
        const uint32_t region = ws_u32(p + 32), captured = ws_u32(p + 40);
        /* job code's bulk data never informs a manager decision: keep the cache for
         * what does (lines, headers, records, lists, counters) */
        if ((xfer == 1 || xfer == 2) && captured == size && captured <= n - 44 && (region != WS_JOB || size <= 128))
            ws_mem_write(t, ea, p + 44, captured);
        if (xfer == 1 && region != WS_JOB && t->reads_n < WS_MAX_READS) {
            t->reads[t->reads_n].ea = ea; t->reads[t->reads_n].size = size; t->reads_n++;
        }
        {   /* a ring: a job's window reaches back only to its own claim */
            struct ws_xfer* x = &t->x[t->x_n++ % WS_MAX_XFER];
            x->seq = t->seq; x->lsa = lsa; x->ea = ea; x->size = size; x->region = (uint8_t)region;
            x->payload = (uint8_t)ws_payload_active(t); x->get = xfer == 1;
        }
        return;
    }
    case WS_ATOMIC: ws_atomic(t, p, n); return;
    case WS_BRANCH: ws_branch(t, p, n); return;
    case WS_KERNEL:
        if (n >= 4 && ws_u32(p) == 2) ws_stop_check(t);   /* exit to the kernel */
        if (n >= 12 && ws_u32(p) == 1) {   /* select: r3 = {wid, status} */
            t->last_poll_status = ws_u32(p + 8);
            t->poll_other = ws_u32(p + 4) != t->wid;
            if (ws_u32(p + 8) == 4) t->flag_owed = 1;
        }
        return;
    case WS_RUN_END: {
        t->active = 0;
        ws_owed(t);
        if (t->r5 == 4 && !t->instance_writes) ws_bad(t, WSC_READY_FLAG, "flag entry without a ready write", 0, 0, 0);
        if (!t->bad && !t->unk) {
            WS_ADD(runs_identical, 1);
            if (t->hint) WS_ADD(runs_with_hints, 1);
        }
        const uint64_t now = ws_now();
        uint64_t next = __atomic_load_n(&s_ws.next_print_ns, __ATOMIC_RELAXED);
        if (!next) __atomic_compare_exchange_n(&s_ws.next_print_ns, &next, now + 5000000000ull, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        else if (now >= next && __atomic_compare_exchange_n(&s_ws.next_print_ns, &next, now + 5000000000ull, 0,
                                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            spurs_wwsjm_shadow_print();
        return;
    }
    default: return;
    }
}
