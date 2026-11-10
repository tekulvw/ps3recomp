/* The encode queue (PS3RECOMP_RSX_ENCODE_THREAD) against a recording backend,
 * with no GPU: what a backend sees through the queue is what it sees called
 * directly, in the same order for everything recorded in stream order, made
 * on the encode thread; creation is made at once on the caller; calls with a
 * result wait for what was queued before them; arguments are copied when the
 * call is made; tickets complete as the backend completes them; the byte ring
 * wraps; and the register-file engine driving it produces the same backend
 * calls with the queue as without. Synthetic inputs only. */
#define main engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_encode_queue.h"
#include <pthread.h>
#include <stdatomic.h>

static int checks, failures;
#define T(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 30) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* ---- a backend that writes down what it is asked ------------------------- */

enum { L_IMMEDIATE = 1, L_QUEUED = 2, L_ORDERED = 4 };
typedef struct { char what[16]; u32 a, b; u64 sum; int worker; int cls; } Entry;
#define LOG_MAX 400000
static Entry* g_log;
static int g_n;
static pthread_t g_main;
static u32 g_next = 1;
static u64 g_gpu_done;                 /* tickets the "GPU" has completed */
static u64 g_issued;
static int g_refuse_rebind, g_fail_submit;
static u32 g_readback_seen;            /* entries logged when the readback ran */

static u64 sum_bytes(const void* p, u64 n)
{
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; p && i < n; i++) h = (h ^ ((const u8*)p)[i]) * 1099511628211ull;
    return h;
}
static void note(const char* what, int cls, u32 a, u32 b, u64 sum)
{
    if (g_n >= LOG_MAX) return;
    Entry* e = &g_log[g_n++];
    memset(e, 0, sizeof *e);
    snprintf(e->what, sizeof e->what, "%s", what);
    e->a = a; e->b = b; e->sum = sum; e->cls = cls;
    e->worker = !pthread_equal(pthread_self(), g_main);
}
static int  r_init(void* u, u32 w, u32 h) { (void)u; note("init", L_ORDERED, w, h, 0); return 0; }
static void r_shutdown(void* u) { (void)u; note("shutdown", L_ORDERED, 0, 0, 0); }
static void r_submit_wait(void* u, u32 r) { (void)u; note("submit_wait", L_ORDERED, r, 0, 0); g_gpu_done = g_issued; }
static u32  r_tex_create(void* u, rsx_be_format f, u32 w, u32 h, u32 m, u32 fa, u32 re, u32 rf)
{ (void)u; (void)f; (void)m; (void)fa; (void)re; (void)rf; const u32 hd = g_next++; note("tex_create", L_IMMEDIATE, hd, w * h, 0); return hd; }
static void r_tex_upload(void* u, u32 t, u32 f, u32 m, u32 w, u32 h, const void* s, u32 rb, u32 rows)
{ (void)u; (void)f; (void)m; (void)w; (void)h; note("tex_upload", L_IMMEDIATE, t, rb * rows, sum_bytes(s, (u64)rb * rows)); }
static void r_release(void* u, u32 h) { (void)u; note("release", L_QUEUED, h, 0, 0); }
static u32  r_color_create(void* u, rsx_be_format f, u32 w, u32 h, const void* seed, u32 rb)
{ (void)u; (void)f; const u32 hd = g_next++; note("color_create", L_IMMEDIATE, hd, w * h, sum_bytes(seed, (u64)rb * h)); return hd; }
static void r_color_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, const void* rows, u32 rb)
{ (void)u; (void)x; (void)y; (void)w; note("color_write", L_QUEUED, s, h, sum_bytes(rows, (u64)rb * h)); }
static u32  r_view(void* u, u32 s, u32 re, u32 f) { (void)u; (void)f; const u32 hd = g_next++; note("view", L_IMMEDIATE, s, re, hd); return hd; }
static u32  r_depth_create(void* u, u32 w, u32 h) { (void)u; const u32 hd = g_next++; note("depth_create", L_IMMEDIATE, hd, w * h, 0); return hd; }
static u32  r_depth_snapshot(void* u, u32 d, u32 w, u32 h) { (void)u; (void)w; (void)h; const u32 hd = g_next++; note("depth_snap", L_ORDERED, d, hd, (u64)g_n); return hd; }
static u32  r_pipe_create(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                          const rsx_vertex_layout_plan* l, u32 st, rsx_be_format f, u32 n)
{ (void)u; (void)rs; (void)l; (void)f; (void)n; const u32 hd = g_next++; note("pipe_create", L_IMMEDIATE, hd, st, sum_bytes(vs, vs ? strlen(vs) : 0) ^ sum_bytes(ps, ps ? strlen(ps) : 0)); return hd; }
static void r_bind_targets(void* u, const u32* s, u32 n, u32 d) { (void)u; note("targets", L_QUEUED, n ? s[0] : 0, d, sum_bytes(s, n * 4u) + n); }
static void r_bind_pipe(void* u, u32 p) { (void)u; note("pipeline", L_QUEUED, p, 0, 0); }
static void r_bind_vs(void* u, const void* d, u32 n) { (void)u; note("vs", L_QUEUED, n, 0, sum_bytes(d, n)); }
static int  r_rebind_vs(void* u) { (void)u; note("rebind", L_QUEUED, 0, 0, 0); return !g_refuse_rebind; }
static void r_bind_vs_rows(void* u, const void* d, u32 n, u32 first, u32 rows)
{
    (void)u;
    const u8* p = (const u8*)d;
    note("vs_rows", L_QUEUED, first, rows, sum_bytes(p + first * 16u, rows * 16u) ^ sum_bytes(p + n - 32u, 32u));
}
static void r_bind_ps(void* u, const void* d, u32 n) { (void)u; note("ps", L_QUEUED, n, 0, sum_bytes(d, n)); }
static void r_bind_tex(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; note("textures", L_QUEUED, m, t ? t[0] : 0, sum_bytes(t, t ? RSX_BE_MAX_TEXTURES * 4u : 0) ^ sum_bytes(s, s ? RSX_BE_MAX_TEXTURES * sizeof *s : 0)); }
static void r_bind_vtex(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; note("vtextures", L_QUEUED, m, 0, sum_bytes(t, t ? RSX_BE_MAX_VERTEX_TEXTURES * 4u : 0) ^ sum_bytes(s, s ? RSX_BE_MAX_VERTEX_TEXTURES * sizeof *s : 0)); }
static void r_viewport(void* u, float x, float y, float w, float h) { (void)u; float f[4] = { x, y, w, h }; note("viewport", L_QUEUED, 0, 0, sum_bytes(f, sizeof f)); }
static void r_scissor(void* u, u32 x, u32 y, u32 w, u32 h) { (void)u; note("scissor", L_QUEUED, x | (y << 16), w | (h << 16), 0); }
static void r_stencil(void* u, u32 r) { (void)u; note("stencil", L_QUEUED, r, 0, 0); }
static void r_bounds(void* u, float a, float b) { (void)u; float f[2] = { a, b }; note("bounds", L_QUEUED, 0, 0, sum_bytes(f, sizeof f)); }
static void r_draw(void* u, rsx_topology t, const void* v, u32 n, u32 st, const u32* idx, u32 ni)
{ (void)u; note("draw", L_QUEUED, n | ((u32)t << 28), ni, sum_bytes(v, (u64)n * st) ^ (sum_bytes(idx, idx ? (u64)ni * 4u : 0) * 3u)); }
static void r_clear_color(void* u, u32 s, const float c[4]) { (void)u; note("clear_color", L_QUEUED, s, 0, sum_bytes(c, 16)); }
static void r_clear_ds(void* u, u32 d, u32 f, float z, u8 st) { (void)u; note("clear_ds", L_QUEUED, d, f | ((u32)st << 8), sum_bytes(&z, 4)); }
static void r_present(void* u, u32 s) { (void)u; note("present", L_QUEUED, s, 0, 0); }
static void r_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{ (void)u; (void)x; (void)y; (void)pitch; g_readback_seen = (u32)g_n; note("readback", L_ORDERED, s, w * h, 0); if (out) memset(out, 0x5A, 4); }
static u32  r_snap(void* u, u32 s) { (void)u; const u32 hd = g_next++; note("snap_whole", L_ORDERED, s, hd, 0); return hd; }
static u32  r_snap_alloc(void* u, u32 s, u32* token) { (void)u; const u32 hd = g_next++; *token = hd * 7u; note("snap_alloc", L_IMMEDIATE, s, hd, 0); return hd; }
static void r_snap_copy(void* u, u32 hd, u32 s, u32 token) { (void)u; note("snap_copy", L_QUEUED, s, hd, token); }
static u64  r_submit_async(void* u, u32 r)
{ (void)u; note("submit_async", L_QUEUED, r, 0, 0); return g_fail_submit ? 0 : ++g_issued; }
static int  r_status(void* u, u64 t, int wait)
{
    (void)u;
    if (!t || t > g_issued) return -1;
    if (wait) g_gpu_done = g_gpu_done > t ? g_gpu_done : t;
    return t <= g_gpu_done ? 1 : 0;
}

static rsx_draw_backend g_rec = {
    .init = r_init, .shutdown = r_shutdown, .submit_and_wait = r_submit_wait,
    .texture_create = r_tex_create, .texture_upload = r_tex_upload, .texture_release = r_release,
    .color_target_create = r_color_create, .color_target_release = r_release, .color_target_write = r_color_write,
    .surface_view = r_view, .depth_target_create = r_depth_create, .depth_target_release = r_release,
    .depth_snapshot = r_depth_snapshot, .pipeline_create = r_pipe_create, .pipeline_release = r_release,
    .bind_targets = r_bind_targets, .bind_pipeline = r_bind_pipe, .bind_vs_constants = r_bind_vs,
    .bind_ps_constants = r_bind_ps, .bind_textures = r_bind_tex, .bind_vertex_textures = r_bind_vtex,
    .set_viewport = r_viewport, .set_scissor = r_scissor, .set_stencil_ref = r_stencil,
    .set_depth_bounds = r_bounds, .draw = r_draw, .clear_color = r_clear_color,
    .clear_depth_stencil = r_clear_ds, .present = r_present, .readback = r_readback,
    .color_snapshot = r_snap, .submit_async = r_submit_async, .completion_status = r_status,
    .rebind_vs_constants = r_rebind_vs, .bind_vs_constant_rows = r_bind_vs_rows,
};
static const rsx_encq_hooks g_hooks = { r_snap_alloc, r_snap_copy };

static void reset_log(void) { g_n = 0; g_next = 1; g_gpu_done = 0; g_issued = 0; g_refuse_rebind = 0; g_fail_submit = 0; }

/* The calls, through `be`: binds and draws whose argument buffers are
 * rewritten as soon as each call returns, creation, releases, a readback
 * that must follow everything before it, tickets and a present. */
static u8 g_big[3u << 20];
static void script(const rsx_draw_backend* be, int rounds)
{
    void* u = be->user;
    float verts[64]; u32 idx[12]; u8 vs[(RSX_DSP_NUM_CONSTANTS + 2) * 16]; float ps[8];
    u32 tex[RSX_BE_MAX_TEXTURES]; rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    memset(samp, 0, sizeof samp);
    T(be->init(u, 640, 360) == 0, "init");
    const u32 rt = be->color_target_create(u, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    const u32 z = be->depth_target_create(u, 64, 64);
    const u32 pipe = be->pipeline_create(u, "vs", "ps", NULL, NULL, 16, RSX_BE_FMT_R8G8B8A8, 1);
    u64 tickets[64]; u32 nt = 0;
    for (int r = 0; r < rounds; r++) {
        for (u32 i = 0; i < 64; i++) verts[i] = (float)(r * 64 + i);
        for (u32 i = 0; i < 12; i++) idx[i] = (u32)(r + i) % 4u;
        for (u32 i = 0; i < sizeof vs; i++) vs[i] = (u8)(r * 13 + i);
        for (u32 i = 0; i < 8; i++) ps[i] = (float)(r + i);
        const u32 t = be->texture_create(u, RSX_BE_FMT_R8G8B8A8, 4, 4, 1, 1, 0, 0);
        u8 texels[64]; memset(texels, r & 0xFF, sizeof texels);
        be->texture_upload(u, t, 0, 0, 4, 4, texels, 16, 4);
        for (u32 i = 0; i < RSX_BE_MAX_TEXTURES; i++) tex[i] = i == 0 ? t : 0;
        const u32 view = be->surface_view(u, rt, 0xAAE4u, 0x85u);
        be->bind_targets(u, &rt, 1, z);
        be->bind_pipeline(u, pipe);
        if (r % 3 == 2 && be->rebind_vs_constants && be->rebind_vs_constants(u)) { /* reused */ }
        else if (r % 3 == 1) be->bind_vs_constant_rows(u, vs, sizeof vs, 3, 5);
        else be->bind_vs_constants(u, vs, sizeof vs);
        be->bind_ps_constants(u, ps, sizeof ps);
        be->bind_textures(u, tex, samp, 1u);
        be->bind_vertex_textures(u, tex, samp, 0u);
        be->set_viewport(u, 0, 0, 64, 64);
        be->set_scissor(u, (u32)r % 7u, 0, 32, 32);
        be->set_stencil_ref(u, (u32)r & 0xFF);
        be->set_depth_bounds(u, 0.0f, 1.0f);
        be->draw(u, RSX_TOPOLOGY_TRIANGLES, verts, 4, 64, idx, 12);
        /* The engine reuses its buffers straight away. */
        memset(verts, 0xEE, sizeof verts); memset(idx, 0xEE, sizeof idx); memset(vs, 0xEE, sizeof vs);
        const float c[4] = { (float)r, 0.5f, 0.25f, 1.0f };
        be->clear_color(u, rt, c);
        be->clear_depth_stencil(u, z, 3u, 1.0f, (u8)r);
        const u32 snap = be->color_snapshot(u, rt);
        T(snap != 0, "snapshot handle");
        if (r % 5 == 0) {
            u8 rows[256]; memset(rows, r, sizeof rows);
            be->color_target_write(u, rt, 0, 0, 8, 8, rows, 32);
        }
        if (r % 2 == 1) {
            /* A big one: the byte ring wraps over a few rounds. */
            memset(g_big, r, sizeof g_big);
            be->draw(u, RSX_TOPOLOGY_TRIANGLES, g_big, (u32)(sizeof g_big / 64u), 64, NULL, 0);
        }
        be->texture_release(u, t);
        be->texture_release(u, view);
        be->color_target_release(u, snap);
        if (r % 4 == 0 && nt < 64) tickets[nt++] = be->submit_async(u, 1);
        if (r % 16 == 15) {
            u32 px = 0;
            be->readback(u, rt, 0, 0, 1, 1, &px, 4);
            T(px == 0x5A5A5A5Au, "readback output");
        }
        if (r % 8 == 7) be->present(u, rt);
    }
    for (u32 i = 0; i < nt; i++) T(be->completion_status(u, tickets[i], 1) == 1, "ticket %u completes", i);
    be->submit_and_wait(u, 0);
    be->shutdown(u);
}

/* The entries of class `cls`, in order, as a list to compare. */
static int same_sequence(const Entry* a, int na, const Entry* b, int nb, int cls)
{
    int i = 0, j = 0;
    for (;;) {
        while (i < na && !(a[i].cls & cls)) i++;
        while (j < nb && !(b[j].cls & cls)) j++;
        if (i == na || j == nb) return i == na && j == nb;
        if (strcmp(a[i].what, b[j].what) || a[i].a != b[j].a || a[i].b != b[j].b || a[i].sum != b[j].sum) {
            printf("  differ at %d/%d: %s %u %u vs %s %u %u\n", i, j, a[i].what, a[i].a, a[i].b, b[j].what, b[j].a, b[j].b);
            return 0;
        }
        i++; j++;
    }
}

static void test_against_direct(void)
{
    printf("-- the same calls with and without the queue\n");
    static Entry direct[LOG_MAX];
    reset_log();
    unsetenv("PS3RECOMP_RSX_ENCODE_THREAD");
    T(rsx_encq_wrap(&g_rec, &g_hooks) == NULL, "no queue unless asked");
    /* Direct, with the split snapshot's halves as the queue would make them. */
    rsx_draw_backend direct_be = g_rec;
    script(&direct_be, 200);
    const int nd = g_n;
    memcpy(direct, g_log, sizeof(Entry) * (size_t)nd);
    int worker = 0;
    for (int i = 0; i < nd; i++) worker |= direct[i].worker;
    T(!worker, "direct: every call on the caller");

    reset_log();
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    const rsx_draw_backend* q = rsx_encq_wrap(&g_rec, &g_hooks);
    T(q != NULL, "wrapped");
    script(q, 200);
    rsx_encq_unwrap();
    const int nq = g_n;
    /* Queued work, in stream order, made on the encode thread. The direct
     * run took whole snapshots; the queued one split them, so the stream
     * minus snapshots must match, and each round has its copy in place. */
    int queued_on_worker = 1, immediate_on_caller = 1, ordered_on_caller = 1, copies = 0;
    for (int i = 0; i < nq; i++) {
        if (g_log[i].cls == L_QUEUED && strcmp(g_log[i].what, "snap_copy")) queued_on_worker &= g_log[i].worker;
        if (g_log[i].cls == L_IMMEDIATE) immediate_on_caller &= !g_log[i].worker;
        if (g_log[i].cls == L_ORDERED) ordered_on_caller &= !g_log[i].worker;
        copies += !strcmp(g_log[i].what, "snap_copy");
    }
    T(queued_on_worker, "queued calls run on the encode thread");
    T(immediate_on_caller && ordered_on_caller, "creation and calls with a result run on the caller");
    T(copies == 200, "every snapshot's copy is queued (%d)", copies);
    /* Strip the snapshot entries and compare the queued stream. */
    static Entry a[LOG_MAX], b[LOG_MAX];
    int na = 0, nb = 0;
    for (int i = 0; i < nd; i++) if (strcmp(direct[i].what, "snap_whole")) a[na++] = direct[i];
    for (int i = 0; i < nq; i++) if (strcmp(g_log[i].what, "snap_copy") && strcmp(g_log[i].what, "snap_alloc")) b[nb++] = g_log[i];
    T(same_sequence(a, na, b, nb, L_QUEUED), "queued calls: same order, arguments and bytes");
    /* Creation hands out handles in the order it is asked for. */
    int ia = 0, ib = 0, created_ok = 1;
    for (;;) {
        while (ia < nd && (direct[ia].cls != L_IMMEDIATE)) ia++;
        while (ib < nq && (g_log[ib].cls != L_IMMEDIATE || !strcmp(g_log[ib].what, "snap_alloc"))) ib++;
        if (ia == nd || ib == nq) break;
        created_ok &= !strcmp(direct[ia].what, g_log[ib].what) && direct[ia].b == g_log[ib].b && direct[ia].sum == g_log[ib].sum;
        ia++; ib++;
    }
    T(created_ok, "creation: the same calls with the same bytes");
    /* A snapshot's copy comes after the draw before it and before the next
     * round's binds: its place among the queued calls. */
    int copy_after_draw = 1;
    for (int i = 0, last = 0; i < nq; i++) {
        if (!strcmp(g_log[i].what, "clear_ds")) last = 1;
        else if (!strcmp(g_log[i].what, "snap_copy")) { copy_after_draw &= last; last = 0; }
    }
    T(copy_after_draw, "each copy follows the clears before it in stream order");
    /* Every readback ran after everything queued before it: as many queued
     * calls ahead of each readback as in the direct run. */
    int ordered_ok = 1, readbacks = 0;
    for (int i = 0, j = 0, qa = 0, qb = 0;;) {
        while (i < na && strcmp(a[i].what, "readback")) qa += a[i++].cls == L_QUEUED;
        while (j < nb && strcmp(b[j].what, "readback")) qb += b[j++].cls == L_QUEUED;
        if (i == na || j == nb) { ordered_ok &= i == na && j == nb; break; }
        ordered_ok &= qa == qb;
        readbacks++; i++; j++;
    }
    T(ordered_ok && readbacks == 12, "readbacks after the work before them (%d)", readbacks);
    rsx_encq_stats st; rsx_encq_get_stats(&st);
    T(st.ops > 0 && st.immediates > 0 && st.snapshots_split == 200 && st.tickets == 50,
      "counters (ops %llu immediates %llu snapshots %llu tickets %llu)", st.ops, st.immediates, st.snapshots_split, st.tickets);
    T(st.payload_bytes > 2ull * (64u << 20), "the byte ring went round more than twice (%llu MB)", st.payload_bytes >> 20);
}

/* A rebind the backend refuses: the encode thread binds what it kept. */
static void test_rebind(void)
{
    printf("-- rebind refused\n");
    reset_log();
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    const rsx_draw_backend* q = rsx_encq_wrap(&g_rec, &g_hooks);
    q->init(q->user, 8, 8);
    u8 vs[(RSX_DSP_NUM_CONSTANTS + 2) * 16];
    for (u32 i = 0; i < sizeof vs; i++) vs[i] = (u8)(i * 3u);
    const u64 want = sum_bytes(vs, sizeof vs);
    T(!q->rebind_vs_constants(q->user), "nothing to reuse before a bind");
    q->bind_vs_constants(q->user, vs, sizeof vs);
    memset(vs, 0, sizeof vs);
    g_refuse_rebind = 1;
    T(q->rebind_vs_constants(q->user) == 1, "a rebind is answered at once");
    rsx_encq_drain();
    int binds = 0, last_ok = 0;
    for (int i = 0; i < g_n; i++) if (!strcmp(g_log[i].what, "vs")) { binds++; last_ok = g_log[i].sum == want; }
    T(binds == 2 && last_ok, "refused: the kept bytes are bound again (%d binds)", binds);
    q->shutdown(q->user);
    rsx_encq_unwrap();
}

/* Tickets: pending until the backend completes them, failure when the
 * backend's submission fails. */
static void test_tickets(void)
{
    printf("-- tickets\n");
    reset_log();
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    const rsx_draw_backend* q = rsx_encq_wrap(&g_rec, &g_hooks);
    q->init(q->user, 8, 8);
    const u64 a = q->submit_async(q->user, 1), b = q->submit_async(q->user, 1);
    T(a && b && b == a + 1, "tickets in order");
    rsx_encq_drain();
    T(q->completion_status(q->user, b, 0) == 0, "pending while the backend says so");
    T(q->completion_status(q->user, b + 5, 0) == -1, "an unknown ticket is a failure");
    g_gpu_done = a;
    int st = 0;
    for (int k = 0; k < 1000 && st != 1; k++) st = q->completion_status(q->user, a, 0);
    T(st == 1, "seen complete once the backend completes it");
    T(q->completion_status(q->user, b, 1) == 1, "waiting completes it");
    g_fail_submit = 1;
    const u64 c = q->submit_async(q->user, 1);
    rsx_encq_drain();
    T(c && q->completion_status(q->user, c, 1) == -1, "a failed submission fails its ticket");
    T(q->submit_async(q->user, 1) == 0, "and the queue refuses tickets after it");
    q->shutdown(q->user);
    rsx_encq_unwrap();
}

/* rsx_encq_call and rsx_encq_wait_done keep their place in the stream. */
static atomic_int g_calls;
static void bump(void* arg) { (void)arg; note("call", L_QUEUED, (u32)atomic_fetch_add(&g_calls, 1), 0, 0); }
static void test_calls(void)
{
    printf("-- calls in order\n");
    reset_log();
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    const rsx_draw_backend* q = rsx_encq_wrap(&g_rec, &g_hooks);
    q->init(q->user, 8, 8);
    float c[4] = { 1, 2, 3, 4 };
    q->clear_color(q->user, 1, c);
    const u64 place = rsx_encq_call(bump, NULL);
    q->clear_color(q->user, 2, c);
    rsx_encq_wait_done(place);
    T(atomic_load(&g_calls) == 1, "waited for the call");
    rsx_encq_drain();
    int order = g_n >= 4 && !strcmp(g_log[1].what, "clear_color") && !strcmp(g_log[2].what, "call") &&
                !strcmp(g_log[3].what, "clear_color");
    T(order, "between the two clears");
    q->shutdown(q->user);
    rsx_encq_unwrap();
    T(!rsx_encq_active(), "stopped");
    T(rsx_encq_call(bump, NULL) == 0 && atomic_load(&g_calls) == 2, "made at once when stopped");
}

/* The engine through the queue: the backend sees the same stream. */
static void engine_script(const rsx_draw_backend* be)
{
    rsx_draw_engine_set_backend(be);
    T(rsx_draw_engine_init(1280, 720) == 0, "engine init");
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    for (u32 f = 0; f < 6; f++) {
        for (u32 i = 0; i < 40; i++) {
            float* v = (float*)(vm_base + VTX_OFFSET);
            for (u32 k = 0; k < 12; k++) v[k] = (float)(f * 100 + i + k);
            memset(vm_base + TEX_OFFSET, (int)(f * 7 + (i / 10)), 64);
            m(M_VP_UPLOAD_CONST_ID, i % 20u);
            m(M_VP_UPLOAD_CONST, 0x3F800000u + i);
            m(M_SCISSOR_H, (i * 3u) | (16u << 16));
            if (i % 9 == 4) { m(M_CLEAR_BUFFERS, 0xF0); }
            draw_triangle();
        }
        rsx_draw_engine_present();
    }
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
}
static void test_engine(void)
{
    printf("-- the engine through the queue\n");
    static Entry direct[LOG_MAX];
    reset_log();
    engine_script(&g_rec);
    const int nd = g_n;
    memcpy(direct, g_log, sizeof(Entry) * (size_t)nd);
    reset_log();
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    const rsx_draw_backend* q = rsx_encq_wrap(&g_rec, &g_hooks);
    engine_script(q);
    rsx_encq_unwrap();
    int draws = 0;
    for (int i = 0; i < nd; i++) draws += !strcmp(direct[i].what, "draw");
    T(draws == 240, "the engine drew (%d)", draws);
    T(same_sequence(direct, nd, g_log, g_n, L_QUEUED), "the backend sees the same queued stream");
    T(same_sequence(direct, nd, g_log, g_n, L_IMMEDIATE), "and the same creation");
}

int main(void)
{
    g_main = pthread_self();
    g_log = (Entry*)calloc(LOG_MAX, sizeof *g_log);
    vm_base = calloc(1, GUEST_BYTES); ppu_vm_size = GUEST_BYTES;
    if (!g_log || !vm_base) return 2;
    test_against_direct();
    test_rebind();
    test_tickets();
    test_calls();
    test_engine();
    printf("encode queue: checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
