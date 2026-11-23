/* The channel operations that only move a word between the SPU program and a
 * field of its own context, written once here for the runtime's channel
 * entries (spu_wrch, spu_rdch, mfc_channel_write) and for lifted code.
 *
 * Latched MFC parameters. The six writes a DMA command is preceded by (local
 * store address, high and low effective address, size, tag) and the tag mask
 * are plain latches: the value is kept in the context until MFC_Cmd or a tag
 * update reads it, nothing else happens. The tag keeps its low five bits. The
 * three status reads (tag status, tag mask, atomic status) return what the
 * last command or tag update left in the context and never wait. Everything
 * else -- the command channel itself, tag updates, list-stall channels,
 * mailboxes, signals, events, the decrementer -- has side effects or may block
 * and stays with the full channel entries.
 *
 * Lifted code (spu_lifter.py --inline-channels) emits, for a channel number
 * fixed by the instruction, `if (spu_channel_inline_on()) <inline op>; else
 * <today's call>`. The inline op is taken only when PS3RECOMP_SPU_CHANNEL_INLINE=1
 * is set and none of the channel diagnostics in spu_fast_path_env.h is armed
 * (resolved once, spu_fast_path_resolve in spu_context.h), so an armed
 * diagnostic such as SPU_CHHIST sees every latch write and status read exactly
 * as before. The switch is independent of PS3RECOMP_MFC_FAST. */
#ifndef SPU_CHANNEL_INLINE_H
#define SPU_CHANNEL_INLINE_H

/* 1 when `channel` is a latch and the preferred word of `value` was stored. */
static inline int spu_mfc_latch_write(spu_context* ctx, uint32_t channel, u128 value)
{
    const uint32_t v = value._u32[0];
    switch (channel) {
    case MFC_LSA:       ctx->mfc_lsa = v;        return 1;
    case MFC_EAH:       ctx->mfc_eah = v;        return 1;
    case MFC_EAL:       ctx->mfc_eal = v;        return 1;
    case MFC_Size:      ctx->mfc_size = v;       return 1;
    case MFC_TagID:     ctx->mfc_tag = v & 0x1F; return 1;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v;   return 1;
    default:            return 0;
    }
}

/* 1 when `channel` is latched status; *out then holds its current value. */
static inline int spu_mfc_latched_read(const spu_context* ctx, uint32_t channel, uint32_t* out)
{
    switch (channel) {
    case MFC_RdTagStat:    *out = ctx->mfc_tag_status; return 1;
    case MFC_RdTagMask:    *out = ctx->mfc_tag_mask;   return 1;
    case MFC_RdAtomicStat: *out = ctx->atomic_stat;    return 1;
    default:               return 0;
    }
}

/* The lifted form of `rdch` on a latched status channel: the value in the
 * preferred word, the other words zero. */
static inline u128 spu_rdch_latched(const spu_context* ctx, uint32_t channel)
{
    uint32_t v = 0;
    (void)spu_mfc_latched_read(ctx, channel, &v);
    return spu_make_preferred_u32(v);
}

/* One load and one compare; -1 (unresolved) selects the call. */
static inline int spu_channel_inline_on(void)
{
    return g_spu_dispatch_gates.channel_inline > 0;
}

#endif /* SPU_CHANNEL_INLINE_H */
