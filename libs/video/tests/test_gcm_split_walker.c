/* The split FIFO walker (PS3RECOMP_RSX_SPLIT_WALKER, gcm_walker_split.inc):
 * the decode thread and the record thread against the single-thread walker.
 *
 * One binary, two forms: `off` walks on one thread, `on` with the switch.
 * Both print the same log lines (tools/test_gcm_split_walker.py compares
 * them), and the `on` form also checks what only the split can get wrong:
 *
 *   1. equivalence: one composite stream (methods, draws, NV406E and backend
 *      labels, SET_REFERENCE, a report, an NV0039 copy, a 2D state method,
 *      a user command, CALL/RET, a JUMP, a flip) gives the same engine calls
 *      in the same order, the same guest memory, and every engine call is
 *      made on the record thread;
 *   2. a fast guest writer: a thread that poisons the vertex bytes the
 *      moment it sees the label (then the ref register, then the flip
 *      completion, then get == put) must never beat the draw before the
 *      release, with the record thread delayed at random;
 *   3. ring reuse: the FIFO words are overwritten the moment the decoder is
 *      past them; the recorded calls must be the original words;
 *   4. a guest wait (cellGcmGetReport) while the record thread is busy
 *      returns within one record, not after the whole queue;
 *   5. an IO unmap and cellGcmTerminate drain the queue first;
 *   6. a resync rescues fences in order behind queued work; checked REF
 *      carries its receipts to the record thread;
 *   7. a random stream of 30k packets gives the same call hash.
 *
 * Synthetic throughout: a stub engine that copies "vertex bytes" at END, a
 * stub backend whose tickets complete on demand, no GPU, no guest content. */
#include "cellGcmSys.c"
#ifdef GCM_SPLIT_REFERENCE_BUILD
/* Built against a walker without the split (origin/main): the off form only. */
static _Thread_local int s_on_record_thread;
static void gcm_split_drain(void) {}
static void gcm_split_stop(void) {}
#endif
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

/* The writer is the emulated guest: it polls label, ref and get words and
 * rewrites its vertex buffer with plain loads and stores, exactly as guest
 * code does through vm_base. Those are the accesses the fixture exists to
 * race against the record thread, in wall time; they carry no happens-before
 * edge by design, so ThreadSanitizer does not instrument this side. The
 * walker's own two threads stay fully instrumented. */
#if defined(__clang__)
#define GUEST_SIDE __attribute__((no_sanitize("thread")))
#elif defined(__GNUC__)
#define GUEST_SIDE __attribute__((no_sanitize_thread))
#else
#define GUEST_SIDE
#endif
/* The guest side's own loads and stores: direct volatile accesses inside
 * uninstrumented functions, so neither the compiler's instrumentation nor the
 * memcpy interceptor sees them (the names need no symbolizer). Big-endian
 * words, as the guest writes them. */
GUEST_SIDE static u32 guest_ld32(u32 ea) { const u32 v = *(const volatile u32*)(vm_base + ea); return __builtin_bswap32(v); }
GUEST_SIDE static void guest_st32(u32 ea, u32 v) { *(volatile u32*)(vm_base + ea) = __builtin_bswap32(v); }

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active = 0;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
u32 g_rsx_last_reference;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_guest_stack(ppu_context* c,const char* t) {(void)c;(void)t;}
void ppu_dump_bctrl_ring(uint32_t id,const char* t) {(void)id;(void)t;}
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a;(void)v;(void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
int rsx_draw_engine_enabled(void) { return 1; }
int rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_live_draw_method_at(u32 m,u32 v,u32 ea){(void)ea;rsx_live_draw_method(m,v);}
void rsx_draw_engine_flush(void) {}
void rsx_live_draw_flush(void) {}
void rsx_state_init(rsx_state* s) { memset(s,0,sizeof(*s)); }
int rsx_process_method(rsx_state* s,u32 m,u32 v) { (void)s; if(m==0x50u)g_rsx_last_reference=v; return 0; }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0; }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0; }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { (void)a;(void)b;(void)c;(void)d; }
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_note_flip(u32 a) {(void)a;}
void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
int rsx_draw_engine_async_available(void){return 1;}
int rsx_draw_engine_in_primitive(void){return 0;}
static u64 issued, completed;
u64 rsx_draw_engine_submit_async(void){return ++issued;}
int rsx_draw_engine_completion_status(u64 ticket,int wait){ if(wait&&ticket>completed)completed=ticket; return ticket<=completed; }
int rsx_draw_engine_checked_ready(void) { return 1; }
int rsx_draw_engine_flush_checked(void) { completed=issued; return 1; }
int ps3_critical_enabled(void){return 0;}
uint64_t ps3_critical_now_ns(void){return 0;}
uint64_t ps3_critical_capture_id(void){return 0;}
void ps3_critical_emit_capture(uint64_t c,uint32_t k,uint64_t o,uint64_t sub,const uint64_t*w,uint32_t n){(void)c;(void)k;(void)o;(void)sub;(void)w;(void)n;}

/* ---- the stub engine: a log, and a "draw" that reads guest bytes ---- */

static int g_on;                       /* the form under test */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_calls, g_wrong_thread, g_hash;
static int g_quiet;                    /* stress: hash only */
static void logline(const char* fmt, ...)
{
    pthread_mutex_lock(&g_log_mu);
    va_list ap; va_start(ap, fmt);
    printf("L "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    pthread_mutex_unlock(&g_log_mu);
}
#define VTX_EA   0x00400000u
#define VTX_BYTES 256u
static _Atomic u32 g_expect_pattern;   /* what the draw must see (0: no check) */
static _Atomic unsigned long long g_poison_hits, g_draws;
static void on_engine_call(u32 m, u32 v, u32 ea)
{
    pthread_mutex_lock(&g_log_mu);
    g_calls++;
    g_hash = (g_hash ^ ((u64)m << 32 ^ v)) * 1099511628211ull;   /* not the EA: the forms lay the stream out differently */
    if (g_on && !s_on_record_thread) g_wrong_thread++;
    pthread_mutex_unlock(&g_log_mu);
    if (!g_quiet) logline("eng m=%04X v=%08X ea=%08X", m, v, ea);
    if ((m & 0x1FFCu) == 0x1808u && v == 0) {
        /* END: the draw copies its vertex bytes out of guest memory now. */
        u32 words[VTX_BYTES / 4];
        for (u32 i = 0; i < VTX_BYTES / 4; i++) words[i] = vm_read32(VTX_EA + i * 4);
        const u32 want = atomic_load(&g_expect_pattern);
        atomic_fetch_add(&g_draws, 1);
        if (want) for (u32 i = 0; i < VTX_BYTES / 4; i++) if (words[i] != want) { atomic_fetch_add(&g_poison_hits, 1); break; }
        if (!g_quiet) logline("draw first=%08X", words[0]);
    }
}
void rsx_draw_engine_method(u32 m, u32 v) { on_engine_call(m, v, 0); }
void rsx_draw_engine_method_at(u32 m,u32 v,u32 ea){ on_engine_call(m, v, ea); }
void rsx_raise_user_cmd(u32 arg) { if (!g_quiet) logline("usercmd %08X", arg); }
void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 len) { if (!g_quiet) logline("dirty %08X+%u", ea, len); }
static _Atomic unsigned g_presents, g_presents_wrong_thread;
void rsx_draw_engine_present_at_flip(u32 buffer_id) {
    atomic_fetch_add(&g_presents, 1);
    if (!s_on_record_thread) atomic_fetch_add(&g_presents_wrong_thread, 1);
    if (!g_quiet) logline("present-at-flip %u", buffer_id);
}

/* ---- the stream ---- */

#define CTRL GCM_CONTROL_GUEST_ADDR
#define IO_EA(io) (((io) >> 20) == 0u ? 0x100000u + (io) : ((io) >> 20) == 1u ? 0x200000u + ((io) & 0xFFFFFu) : 0u)
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static u32 wr(u32 io, u32 w) { raw32(IO_EA(io), w); return io + 4; }
static u32 wr_n(u32 io, const u32* w, u32 n) { for (u32 i = 0; i < n; i++) io = wr(io, w[i]); return io; }
static void set_put(u32 io) { atomic_thread_fence(memory_order_release); raw32(CTRL + 0, io); }
GUEST_SIDE static u32 get_reg(void) { return guest_ld32(CTRL + 4); }
static void walk_to(u32 put)
{
    set_put(put);
    for (int n = 0; n < 10000 && s_fifo_getoff != put; n++) cellGcm_rsx_process_fifo();
    assert(s_fifo_getoff == put);
}
static void drained(void) { gcm_split_drain(); }
GUEST_SIDE static void fill_vtx(u32 pattern) { for (u32 i = 0; i < VTX_BYTES / 4; i++) guest_st32(VTX_EA + i * 4, pattern); }
static u32 draw_words(u32 io)
{
    const u32 w[] = { 0x41808u, 4u, 0x41814u, 0x00030000u, 0x41808u, 0u };
    return wr_n(io, w, 6);
}

/* ---- the fast guest writer ---- */

enum { SIG_NONE, SIG_LABEL, SIG_REF, SIG_FLIP, SIG_GETPUT, SIG_QUIT };
static _Atomic int g_sig_kind;
static _Atomic u32 g_sig_value;
static _Atomic int g_poisoned;
static u32 g_label_ea;
GUEST_SIDE static void* writer_main(void* unused)
{
    (void)unused;
    for (;;) {
        const int kind = atomic_load_explicit(&g_sig_kind, memory_order_acquire);
        if (kind == SIG_QUIT) return NULL;
        if (kind == SIG_NONE) { sched_yield(); continue; }
        const u32 v = atomic_load(&g_sig_value);
        int fire = 0;
        switch (kind) {
        case SIG_LABEL:  fire = guest_ld32(g_label_ea) == v; break;
        case SIG_REF:    fire = guest_ld32(CTRL + 8) == v; break;
        case SIG_FLIP:   fire = *(volatile u32*)&s_fifo_flips_reached == v; break;
        case SIG_GETPUT: fire = get_reg() == guest_ld32(CTRL + 0); break;
        }
        if (!fire) continue;
        fill_vtx(0xBADBAD00u);                  /* the guest reuses its buffer */
        atomic_store(&g_sig_kind, SIG_NONE);
        atomic_store_explicit(&g_poisoned, 1, memory_order_release);
    }
}

/* Random delays before each record, so the writer gets its chances. */
static _Atomic int g_delay_us_max;
static void test_hook(const u32* r, u64 pos)
{
    (void)r; (void)pos;
    const int mx = atomic_load(&g_delay_us_max);
    if (mx <= 0) return;
    static unsigned seed = 12345u;
    const long us = mx > 1 ? (long)(rand_r(&seed) % (unsigned)mx) : mx;
    const struct timespec ts = { 0, us * 1000L };
    nanosleep(&ts, NULL);
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int fails;
/* GCM_SPLIT_TEST_SCALE=<percent>: the writer iterations and the stress packets (sanitizer builds run a fifth). */
static int scale_pct = 100;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* a timed-out run still shows how far it got */
    g_on = argc > 1 && !strcmp(argv[1], "on");
    { const char* e = getenv("GCM_SPLIT_TEST_SCALE"); if (e && atoi(e) > 0) scale_pct = atoi(e); }
    if (g_on) setenv("PS3RECOMP_RSX_SPLIT_WALKER", "1", 1);
    setenv("PS3RECOMP_GCM_FAST_WALK", "1", 1);
    /* The deferred-report constructor ran before main: set its switches as
     * the early-release fixture does (deferred, early release, early sync). */
    memset(&s_reports,0,sizeof(s_reports));s_reports.generation=3;
    s_deferred_reports=1;s_early_release=1;s_early_fifo_sync=1;s_reports.early_mode=1;
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);
    ppu_hle_inject_base=0x3000000; ppu_vm_size=0x4000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1; s_ea_address_table[1]=2; s_ea_address_table[0xE0]=0x20;
    s_gcm_syscall_mode=1;s_config.localAddress=0;s_config.localSize=0x1000000u;
#ifndef GCM_SPLIT_REFERENCE_BUILD
    if (g_on) s_rq.test_hook = test_hook;
#endif

    /* 1. Equivalence. */
    u32 io = 0x1000;
    const u32 sub = 0x80000, jmp = 0x90000;
    g_label_ea = GCM_LABEL_GUEST_BASE + 0x920;
    fill_vtx(0x11111111u);
    { u32 p = sub; const u32 w[] = { 0x41a00u, 0xAAu, 0x41a04u, 0xBBu }; p = wr_n(p, w, 4); wr(p, 0x00020000u); }
    { const u32 w[] = { 0x41a40u, 1u, 0x41a44u, 2u, 0x41a48u, 3u };        io = wr_n(io, w, 6); }   /* plain state */
    io = draw_words(io);
    { const u32 w[] = { 0x40064u, 0x920u, 0x4006cu, 0x1001u };               io = wr_n(io, w, 4); }   /* NV406E label */
    { const u32 w[] = { 0x41d6cu, 0x900u, 0x41d74u, 0x2002u };               io = wr_n(io, w, 4); }   /* backend label */
    { const u32 w[] = { 0x40050u, 0x3003u };                                 io = wr_n(io, w, 2); }   /* SET_REFERENCE */
    { const u32 w[] = { 0x401A8u, 0xBAD68000u, 0x41800u, 0x01000430u };      io = wr_n(io, w, 4); }   /* report */
    for (u32 i = 0; i < 64; i++) raw32(0x200000u + i * 4, 0x5A000000u | i);                          /* NV0039 source */
    { const u32 w[] = { 0x82184u, 0xFEED0000u, 0xFEED0000u, 0x0020230Cu,
                        0x200000u, 0x300000u, 256u, 256u, 256u, 1u, 0x101u, 0u };                      io = wr_n(io, w, 12); }
    { const u32 w[] = { 0x4430Cu, 0x7700u };                                 io = wr_n(io, w, 2); }   /* subch 2: NV3062 dst offset */
    { const u32 w[] = { 0x4EB00u, 0x42u };                                   io = wr_n(io, w, 2); }   /* user command (subch 7, 0xB00) */
    io = wr(io, sub | 2u);                                                                              /* CALL */
    io = wr(io, 0x20000000u | jmp);                                                                     /* JUMP */
    { u32 p = jmp; p = draw_words(p); p = wr(p, GCM_FLIP_MARKER | 1u);
      const u32 w[] = { 0x41a4cu, 9u }; p = wr_n(p, w, 2); io = p; }
    s_fifo_flips_queued = 1;
    walk_to(io); drained();
    logline("flips reached=%u label=%08X blabel=%08X ref=%08X get=%08X", s_fifo_flips_reached,
            vm_read32(g_label_ea), vm_read32(VM_HLE_INJECT_BASE + 0x900), vm_read32(CTRL + 8), get_reg());
    { u64 h = 0; for (u32 i = 0; i < 64; i++) h = h * 31u + vm_read32(0x300000u + i * 4); logline("nv0039 dst hash=%016llX", (unsigned long long)h); }
    logline("report=%08X %08X", vm_read32(0x2000430u + 8), vm_read32(0x2000430u + 12));
    CHECK(get_reg() == io, "get published to put after the walk (%08X vs %08X)", get_reg(), io);
    CHECK(atomic_load(&g_presents) == (unsigned)(g_on ? 1 : 0), "the record thread presents at the flip (%u)", atomic_load(&g_presents));
    if (g_on) CHECK(!g_wrong_thread && !atomic_load(&g_presents_wrong_thread), "engine calls on the record thread only (%llu off it)", g_wrong_thread);
    printf("equivalence: %llu engine calls\n", g_calls);

    /* 2. The fast guest writer. */
    g_quiet = 1;
    pthread_t writer; assert(!pthread_create(&writer, NULL, writer_main, NULL));
    atomic_store(&g_delay_us_max, g_on ? 60 : 0);
    const int kinds[4] = { SIG_LABEL, SIG_REF, SIG_FLIP, SIG_GETPUT };
    const char* names[4] = { "label", "ref", "flip", "get==put" };
    u32 seg = 0x100000u;                               /* io page 1 */
    const int get_decode = getenv("PS3RECOMP_RSX_SPLIT_WALKER_GET") && !strcmp(getenv("PS3RECOMP_RSX_SPLIT_WALKER_GET"), "decode");
    for (int k = 0; k < 4; k++) {
        /* get published by the decoder: a guest that waits for get == put before
         * reusing data is the hazard that mode accepts; the other three releases
         * still must never beat the draw. */
        if (kinds[k] == SIG_GETPUT && g_on && get_decode) { printf("writer get==put: skipped (get published by the decoder)\n"); continue; }
        atomic_store(&g_poison_hits, 0); atomic_store(&g_draws, 0);
        const int iters = 700 * scale_pct / 100;
        for (int i = 1; i <= iters; i++) {
            const u32 pattern = 0x01000000u + (u32)(k << 16) + (u32)i;
            fill_vtx(pattern); atomic_store(&g_expect_pattern, pattern);
            u32 p = seg;
            p = draw_words(p);
            switch (kinds[k]) {
            case SIG_LABEL: { const u32 w[] = { 0x40064u, 0x920u, 0x4006cu, (u32)i }; p = wr_n(p, w, 4); } break;
            case SIG_REF:   { const u32 w[] = { 0x40050u, (u32)i }; p = wr_n(p, w, 2); } break;
            case SIG_FLIP:  p = wr(p, GCM_FLIP_MARKER | 1u); s_fifo_flips_queued = s_fifo_flips_reached + 1u; break;
            case SIG_GETPUT: { const u32 w[] = { 0x41a50u, (u32)i }; p = wr_n(p, w, 2); } break;
            }
            set_put(p);
            atomic_store(&g_poisoned, 0);
            atomic_store(&g_sig_value, kinds[k] == SIG_FLIP ? s_fifo_flips_reached + 1u : (u32)i);
            atomic_store_explicit(&g_sig_kind, kinds[k], memory_order_release);
            for (int n = 0; n < 10000 && s_fifo_getoff != p; n++) cellGcm_rsx_process_fifo();
            assert(s_fifo_getoff == p);
            /* 3. Ring reuse: the decoder is past these words; the guest may
             * overwrite them now. The recorded calls must be the originals. */
            for (u32 q = seg; q < p; q += 4) wr(q, 0x3FFFFFF0u);
            drained();
            for (int n = 0; n < 2000000 && !atomic_load_explicit(&g_poisoned, memory_order_acquire); n++) sched_yield();
            CHECK(atomic_load(&g_poisoned), "%s: the writer saw its release (iteration %d)", names[k], i);
            seg = p;
            if (!atomic_load(&g_poisoned)) { atomic_store(&g_sig_kind, SIG_NONE); break; }
            if (p > 0x1F0000u) seg = 0x100000u, cellGcm_rsx_consumer_begin(), cellGcm_syscall_set_fifo_consumer_body(seg, seg), cellGcm_rsx_consumer_end();
        }
        CHECK(atomic_load(&g_draws) == (unsigned long long)iters, "%s: every draw recorded (%llu of %d)", names[k], atomic_load(&g_draws), iters);
        if (fails) break;
        CHECK(atomic_load(&g_poison_hits) == 0, "%s: a draw read poisoned vertex bytes %llu times", names[k], atomic_load(&g_poison_hits));
        printf("writer %s: %d iterations, %llu poison hits\n", names[k], iters, atomic_load(&g_poison_hits));
    }
    atomic_store(&g_sig_kind, SIG_QUIT); pthread_join(writer, NULL);
    atomic_store(&g_expect_pattern, 0);
    atomic_store(&g_delay_us_max, 0);

#ifndef GCM_SPLIT_REFERENCE_BUILD
    /* 4. A guest wait while the record thread is busy. */
    if (g_on) {
        atomic_store(&g_delay_us_max, 2000);
        u32 p = seg;
        for (int i = 0; i < 60; i++) { const u32 w[] = { 0x41a40u, (u32)i }; p = wr_n(p, w, 2); }
        set_put(p);
        const u64 head0 = atomic_load(&s_rq.head);
        for (int n = 0; n < 100 && s_fifo_getoff != p; n++) cellGcm_rsx_process_fifo();
        /* Only once the record thread is into the queue (holding the lease). */
        for (int n = 0; n < 2000000 && atomic_load(&s_rq.head) == head0; n++) sched_yield();
        const double t0 = now_ms();
        (void)cellGcmGetReport(1, 0x43);
        const double dt = now_ms() - t0;
        CHECK(dt < 50.0, "a guest report wait returned after %.1f ms (the record thread must yield the lease)", dt);
        printf("guest wait: %.1f ms with 60 records of 2 ms queued\n", dt);
        drained(); atomic_store(&g_delay_us_max, 0);
        seg = p;
    }
#endif

#ifndef GCM_SPLIT_REFERENCE_BUILD
    /* 5a. Unmap drains: io page 2 -> ea page 3, 40 methods queued behind a
     * slow record thread, then the page is unmapped. */
    if (g_on) {
        cellGcm_rsx_consumer_begin(); cellGcm_syscall_iomap_consumer_body(0x300000u, 0x200000u, 0x100000u); cellGcm_rsx_consumer_end();
        CHECK(gcm_io2ea(0x200000u) == 0x300000u, "page mapped");
        cellGcm_rsx_consumer_begin(); cellGcm_syscall_set_fifo_consumer_body(0x200000u, 0x200000u); cellGcm_rsx_consumer_end();
        atomic_store(&g_delay_us_max, 1000);
        const unsigned long long before = g_calls;
        u32 p = 0x200000u;
        for (int i = 0; i < 40; i++) { const u32 w[] = { 0x41a44u, (u32)i }; raw32(0x300000u + (p & 0xFFFFFu), w[0]); raw32(0x300000u + (p & 0xFFFFFu) + 4, w[1]); p += 8; }
        set_put(p);
        for (int n = 0; n < 100 && s_fifo_getoff != p; n++) cellGcm_rsx_process_fifo();
        cellGcm_syscall_iounmap(0x200000u, 0x100000u);   /* drains first */
        CHECK(g_calls - before == 40u, "unmap after a drain: %llu of 40 calls recorded", g_calls - before);
        atomic_store(&g_delay_us_max, 0);
        cellGcm_rsx_consumer_begin(); cellGcm_syscall_set_fifo_consumer_body(seg, seg); cellGcm_rsx_consumer_end();
    }
#endif

    /* 6a. A resync rescues fences in order behind queued work. */
    {
        u32 p = seg;
        { const u32 w[] = { 0x40050u, 0xA1u }; p = wr_n(p, w, 2); }
        p = wr(p, 0x20000000u | 0xF00000u);                 /* JUMP into an unmapped page */
        { const u32 w[] = { 0x40050u, 0xA2u }; p = wr_n(p, w, 2); }
        set_put(p);
        for (int n = 0; n < 100 && s_fifo_getoff != p; n++) cellGcm_rsx_process_fifo();
        drained();
        CHECK(vm_read32(CTRL + 8) == 0xA1u, "the fence before the resync published first (%08X)", vm_read32(CTRL + 8));
        { const struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL); }
        cellGcm_ref_on_poll();
        CHECK(vm_read32(CTRL + 8) == 0xA2u, "the rescued fence published next (%08X)", vm_read32(CTRL + 8));
        seg = p;
    }
    /* 6b. Checked REF: the record thread takes the receipt. */
    {
        setenv("PS3RECOMP_GCM_CHECKED_REF", "1", 1);
        u32 p = seg;
        { const u32 w[] = { 0x40050u, 0xC1u }; p = wr_n(p, w, 2); }
        set_put(p);
        for (int n = 0; n < 100 && s_fifo_getoff != p; n++) cellGcm_rsx_process_fifo();
        drained();
        CHECK(s_ref_qtail > 0 && s_ref_receipts[(s_ref_qtail - 1u) % GCM_REF_QLEN] != 0, "a checked reference carries its receipt");
        CHECK(!atomic_load(&s_checked_ref_failed), "checked REF did not fail");
        unsetenv("PS3RECOMP_GCM_CHECKED_REF");
        seg = p;
    }

    /* 7. Stress: a random stream, hashed. */
    {
        unsigned seed = 777u;
        const unsigned long long calls0 = g_calls; g_hash = 0;
        u32 p = seg;
        int packets = 0;
        while (packets < 30000 * scale_pct / 100) {
            if (p > 0x1F0000u) { walk_to(p); drained(); p = seg = 0x100000u; cellGcm_rsx_consumer_begin(); cellGcm_syscall_set_fifo_consumer_body(seg, seg); cellGcm_rsx_consumer_end(); }
            const unsigned r = rand_r(&seed);
            if (r % 50 == 0) p = draw_words(p);
            else if (r % 97 == 0) { const u32 w[] = { 0x40064u, 0x920u, 0x4006cu, r }; p = wr_n(p, w, 4); }
            else if (r % 101 == 0) { const u32 w[] = { 0x40050u, r }; p = wr_n(p, w, 2); }
            else {
                const u32 count = 1u + (r >> 8) % 8u, method = 0x1a00u + ((r >> 12) % 64u) * 4u;
                p = wr(p, (count << 18) | method);
                for (u32 i = 0; i < count; i++) p = wr(p, rand_r(&seed));
            }
            packets++;
            if (packets % 300 == 0) walk_to(p);
        }
        walk_to(p); drained();
        logline("stress calls=%llu hash=%016llX", g_calls - calls0, g_hash);
        seg = p;
    }

#ifndef GCM_SPLIT_REFERENCE_BUILD
    /* 5b. Terminate stops the record thread after draining; a later pass
     * starts it again. */
    if (g_on) {
        atomic_store(&g_delay_us_max, 1000);
        const unsigned long long before = g_calls;
        u32 p = seg;
        for (int i = 0; i < 40; i++) { const u32 w[] = { 0x41a48u, (u32)i }; p = wr_n(p, w, 2); }
        set_put(p);
        for (int n = 0; n < 100 && s_fifo_getoff != p; n++) cellGcm_rsx_process_fifo();
        cellGcmTerminate();
        CHECK(g_calls - before == 40u, "terminate after a drain: %llu of 40 calls recorded", g_calls - before);
        CHECK(!atomic_load(&s_rq.started), "the record thread stopped");
        atomic_store(&g_delay_us_max, 0);
        memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
        s_ea_address_table[0]=1; s_ea_address_table[1]=2; s_ea_address_table[0xE0]=0x20;
        s_config.localAddress=0;s_config.localSize=0x1000000u;
        cellGcm_rsx_consumer_begin(); cellGcm_syscall_set_fifo_consumer_body(p, p); cellGcm_rsx_consumer_end();
        u32 q = p; for (int i = 0; i < 5; i++) { const u32 w[] = { 0x41a48u, 0x100u + (u32)i }; q = wr_n(q, w, 2); }
        walk_to(q); drained();
        CHECK(g_calls - before == 45u, "the record thread restarted (%llu of 45 calls)", g_calls - before);
        CHECK(atomic_load(&s_rq.started), "started again");
        gcm_split_stop();
    }
    if (g_on && s_rq.stats_on) gcm_split_stats(gcm_split_now_ns() + 3000000000ull);
#endif
    printf("%s: %s (%d failed)\n", g_on ? "split walker" : "single-thread walker", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
