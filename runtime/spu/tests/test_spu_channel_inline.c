/* The inline channel form (spu_lifter.py --inline-channels, runtime/spu/
 * spu_channel_inline.h) against the ordinary lift, through the real channel
 * entries, MFC engine and drain. Neutral synthetic program only
 * (tools/test_spu_channel_inline.py).
 *
 *   test_spu_channel_inline <entry>
 *
 * Prints one RESULT line: the resolved switch words, the registers the
 * program read channels into, the MFC latches and statuses, the mailbox, and
 * FNV digests of the register file, local store and guest memory. */
#define main channel_inline_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#include "channel_program.h"

extern void CHANNEL_REGISTER(void);

static uint64_t fnv(const void* p, size_t n)
{
    const unsigned char* b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

int main(int argc, char** argv)
{
    uint32_t entry = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : CHANNEL_BASE;
    ppu_vm_size = (uint32_t)sizeof guest_memory;
    spu_context* ctx = calloc(1, sizeof(*ctx)); CHECK(ctx != NULL);
    spu_context_init(ctx, 0);
    ctx->status = SPU_STATUS_RUNNING;
    ctx->pc = entry;
    for (int i = 0; i < 128; i++)
        for (int j = 0; j < 4; j++) ctx->gpr[i]._u32[j] = 0x01010101u * (uint32_t)(i + 1) + (uint32_t)j;
    ctx->gpr[1] = spu_link(0x3FF00);
    memcpy(ctx->ls + CHANNEL_BASE, channel_program, sizeof(channel_program));
    /* Patterned guest source for the GET, a marked line for the atomic pair. */
    for (uint32_t a = 0; a < 0x400; a++) guest_memory[CHANNEL_SRC_EA + a] = (uint8_t)(a * 7 + (a >> 6));
    for (uint32_t a = 0; a < 0x80; a++) guest_memory[CHANNEL_LINE_EA + a] = (uint8_t)(0xA0 + a);
    for (uint32_t a = 0x3000; a < 0x3080; a++) ctx->ls[a] = (uint8_t)(a ^ 0x5A);

    spu_begin_image(0);
    CHANNEL_REGISTER();
    spu_lifted_fn fn = spu_lifted_lookup(ctx, entry);
    CHECK(fn != NULL);
    if (!fn) return 1;
    int halted = spu_run_with_halt(fn, ctx);

    printf("RESULT inline=%d fast=%d pc=%05x status=%x stop=%x halted=%d",
           g_spu_dispatch_gates.channel_inline, g_spu_dispatch_gates.fast_path,
           ctx->pc & SPU_LS_MASK, ctx->status, ctx->stop_code, halted);
    printf(" lsa=%x eah=%x eal=%x size=%x tag=%x mask=%x stat=%x atomic=%x",
           ctx->mfc_lsa, ctx->mfc_eah, ctx->mfc_eal, ctx->mfc_size, ctx->mfc_tag,
           ctx->mfc_tag_mask, ctx->mfc_tag_status, ctx->atomic_stat);
    printf(" mbox=%u/%x", (unsigned)ctx->ch_out_mbox.count, ctx->ch_out_mbox.count ? ctx->ch_out_mbox.value : 0u);
    for (int r = 11; r <= 31; r++) printf(" r%d=%x", r, ctx->gpr[r]._u32[0]);
    printf(" regs=%016llx ls=%016llx mem=%016llx failed=%d\n",
           (unsigned long long)fnv(ctx->gpr, sizeof ctx->gpr),
           (unsigned long long)fnv(ctx->ls, SPU_LS_SIZE),
           (unsigned long long)fnv(guest_memory, sizeof guest_memory), g_fail);
    return g_fail ? 1 : 0;
}
