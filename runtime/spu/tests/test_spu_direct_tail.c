/* Direct tail calls between lifted functions (spu_lifter.py --direct-tail)
 * against the ordinary lift, through the real drain, channels, interrupts,
 * halt and restart paths. Neutral synthetic program only
 * (tools/test_spu_direct_tail.py).
 *
 *   test_spu_direct_tail <entry> <opts>
 *
 * opts is a comma-separated list, each arming one thing the drain does per
 * transfer, so the guard must send transfers back to the drain:
 *   irq     an event is pending and enabled from the start
 *   lr      the reservation-lost event is in the event mask
 *   pmflow  the policy-module flow trace targets this context
 *   reset   a stack-reset entry is registered (an unrelated pc)
 *   raiser  interrupts get an event raised by another thread mid-run
 *   none    nothing
 * Prints one RESULT line: pc, status, stop code, steps, host depth, the
 * innermost drain return pc, digests of the register file, local store and
 * recent-pc ring, and how many tail calls the guard admitted. */
#define main direct_tail_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#include "direct_tail_program.h"
#include <pthread.h>
#include <time.h>

extern void DIRECT_TAIL_REGISTER(void);
unsigned long long g_spu_direct_tail_count;
extern void spu_register_stack_reset_entry(uint32_t entry, int image_id);

static uint64_t fnv(const void* p, size_t n)
{
    const unsigned char* b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* Raise the enabled event from another thread, the way an MFC/PPU producer
 * does (an atomic or into event_status), once the guest loop is running. */
static spu_context* s_raise_ctx;
static void* raiser(void* unused)
{
    (void)unused;
    struct timespec d = { 0, 2 * 1000 * 1000 };
    nanosleep(&d, NULL);
    __atomic_fetch_or(&s_raise_ctx->event_status, 0x2u, __ATOMIC_SEQ_CST);
    return NULL;
}

int main(int argc, char** argv)
{
    uint32_t entry = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : DIRECT_TAIL_BASE;
    const char* opts = argc > 2 ? argv[2] : "none";
    spu_context* ctx = calloc(1, sizeof(*ctx)); CHECK(ctx != NULL);
    spu_context_init(ctx, 0);
    ctx->status = SPU_STATUS_RUNNING;
    ctx->pc = entry;
    for (int i = 0; i < 128; i++)
        for (int j = 0; j < 4; j++) ctx->gpr[i]._u32[j] = 0x01010101u * (uint32_t)(i + 1) + (uint32_t)j;
    for (int i = 40; i < 48; i++) memset(&ctx->gpr[i], 0, sizeof ctx->gpr[i]);
    ctx->gpr[1] = spu_link(0x3FF00);
    memcpy(ctx->ls + DIRECT_TAIL_BASE, direct_tail_program, sizeof(direct_tail_program));
    memcpy(ctx->ls, direct_tail_vector, sizeof(direct_tail_vector));   /* LS 0: `br handler` */
    for (uint32_t a = 0x30000; a < 0x30200; a++) ctx->ls[a] = (uint8_t)(a * 7);
    if (strstr(opts, "irq")) { ctx->event_mask = 0x2u; ctx->event_status = 0x2u; }
    if (strstr(opts, "lr")) ctx->event_mask |= 0x400u;
    if (strstr(opts, "pmflow")) g_pm_flow_ctx = ctx;
    if (strstr(opts, "reset")) spu_register_stack_reset_entry(0x3FFF0, 0);
    spu_begin_image(0);
    DIRECT_TAIL_REGISTER();
    pthread_t th;
    if (strstr(opts, "raiser")) {
        ctx->event_mask |= 0x2u;
        s_raise_ctx = ctx;
        CHECK(pthread_create(&th, NULL, raiser, NULL) == 0);
    }
    spu_lifted_fn fn = spu_lifted_lookup(ctx, entry);
    CHECK(fn != NULL);
    if (!fn) return 1;
    int halted = spu_run_with_halt(fn, ctx);
    if (strstr(opts, "raiser")) pthread_join(th, NULL);
    uint32_t mark = (uint32_t)ctx->ls[0x30100] << 24 | (uint32_t)ctx->ls[0x30101] << 16 |
                    (uint32_t)ctx->ls[0x30102] << 8 | ctx->ls[0x30103];
    unsigned ring_n = g_spu_recent.n;
    uint32_t ring[32];
    for (unsigned i = 0; i < 32; i++) ring[i] = g_spu_recent.pc[(ring_n + i) & 31];
    printf("RESULT r40=%u r44=%u mark=%u pc=%05x status=%x stop=%x halted=%d steps=%u depth=%u "
           "drain_ret=%05x ring_n=%u ring=%016llx regs=%016llx ls=%016llx tail_on=%d tails=%llu failed=%d\n",
           ctx->gpr[40]._u32[0], ctx->gpr[44]._u32[0], mark, ctx->pc & SPU_LS_MASK, ctx->status,
           ctx->stop_code, halted, (unsigned)ctx->steps, (unsigned)ctx->host_depth,
           ctx->drain_ret_pc & SPU_LS_MASK, ring_n, (unsigned long long)fnv(ring, sizeof ring),
           (unsigned long long)fnv(ctx->gpr, sizeof ctx->gpr),
           (unsigned long long)fnv(ctx->ls, SPU_LS_SIZE), g_spu_direct_tail_on, g_spu_direct_tail_count, g_fail);
    return g_fail ? 1 : 0;
}
