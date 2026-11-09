/* Register promotion (spu_lifter.py --promote-regs) against the ordinary lift
 * and the interpreter, through the real drain, channels, halt and restart
 * paths. Neutral synthetic program only (tools/test_spu_promote_regs.py).
 *
 *   test_spu_promote_regs <entry> <lift|interp> [irq]
 *
 * Prints one RESULT line: pc, status, stop code, an FNV digest of the whole
 * register file and one of local store. */
#define main promote_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#include "promote_program.h"
#include "spu_interp.h"

extern void PROMOTE_REGISTER(void);

static void interp_entry(spu_context* c) { spu_interp_run(c, c->pc); }

static uint64_t fnv(const void* p, size_t n)
{
    const unsigned char* b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

int main(int argc, char** argv)
{
    uint32_t entry = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : PROMOTE_BASE;
    int interp = argc > 2 && !strcmp(argv[2], "interp");
    int irq = argc > 3 && atoi(argv[3]);
    spu_context* ctx = calloc(1, sizeof(*ctx)); CHECK(ctx != NULL);
    spu_context_init(ctx, 0);
    ctx->status = SPU_STATUS_RUNNING;
    ctx->pc = entry;
    for (int i = 0; i < 128; i++)
        for (int j = 0; j < 4; j++) ctx->gpr[i]._u32[j] = 0x01010101u * (uint32_t)(i + 1) + (uint32_t)j;
    ctx->gpr[1] = spu_link(0x3FF00);
    memcpy(ctx->ls + PROMOTE_BASE, promote_program, sizeof(promote_program));
    /* LS 0: the interrupt vector the drain decodes (`br handler`). */
    memcpy(ctx->ls, promote_vector, sizeof(promote_vector));
    for (uint32_t a = 0x30000; a < 0x30200; a++) ctx->ls[a] = (uint8_t)(a * 7);
    if (irq) { ctx->event_mask = 0x2u; ctx->event_status = 0x2u; }
    spu_lifted_fn fn = interp_entry;
    if (!interp) {
        spu_begin_image(0);
        PROMOTE_REGISTER();
        fn = spu_lifted_lookup(ctx, entry);
        CHECK(fn != NULL);
        if (!fn) return 1;
    }
    int halted = spu_run_with_halt(fn, ctx);
    uint32_t irq_mark = (uint32_t)ctx->ls[0x30100] << 24 | (uint32_t)ctx->ls[0x30101] << 16 |
                        (uint32_t)ctx->ls[0x30102] << 8 | ctx->ls[0x30103];
    printf("RESULT r30=%u r34=%u mark=%u pc=%05x status=%x stop=%x halted=%d regs=%016llx ls=%016llx failed=%d\n",
           ctx->gpr[30]._u32[0], ctx->gpr[34]._u32[0], irq_mark,
           ctx->pc & SPU_LS_MASK, ctx->status, ctx->stop_code, halted,
           (unsigned long long)fnv(ctx->gpr, sizeof ctx->gpr),
           (unsigned long long)fnv(ctx->ls, SPU_LS_SIZE), g_fail);
    return g_fail ? 1 : 0;
}
