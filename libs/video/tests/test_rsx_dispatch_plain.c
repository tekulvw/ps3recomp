/* rsx_dispatch_plain_register against rsx_dispatch_method itself.
 *
 * For every register index and a matrix of states (inside and outside
 * BEGIN/END, two argument values, upload load slots set), the real
 * rsx_dispatch_method is called on a dispatcher whose sinks record every
 * call. A register the mirror calls plain must have changed exactly
 * regs[idx] (to the argument) and seen[idx] (by one) and fired no sink, in
 * every state; anything else is unsound and fails. Registers the mirror
 * calls special but that behaved plainly are counted (conservative, allowed)
 * and printed.
 *
 * python3 tools/test_rsx_dispatch_plain.py [--mutants] */
#include "rsx_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_sink_calls;
static void s_clear(void* u, const rsx_dispatch* r, u32 a) { (void)u; (void)r; (void)a; g_sink_calls++; }
static void s_begin(void* u, const rsx_dispatch* r, u32 p) { (void)u; (void)r; (void)p; g_sink_calls++; }
static void s_end(void* u, const rsx_dispatch* r) { (void)u; (void)r; g_sink_calls++; }
static void s_draw_arrays(void* u, const rsx_dispatch* r, u32 f, u32 c) { (void)u; (void)r; (void)f; (void)c; g_sink_calls++; }
static void s_draw_index(void* u, const rsx_dispatch* r, u32 f, u32 c) { (void)u; (void)r; (void)f; (void)c; g_sink_calls++; }
static void s_inline(void* u, const rsx_dispatch* r, const u8* d, u32 b) { (void)u; (void)r; (void)d; (void)b; g_sink_calls++; }
static void s_flip(void* u, const rsx_dispatch* r, u32 a) { (void)u; (void)r; (void)a; g_sink_calls++; }
static void s_immediate(void* u, const rsx_dispatch* r) { (void)u; (void)r; g_sink_calls++; }

int main(void)
{
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear = s_clear; sink.begin = s_begin; sink.end = s_end;
    sink.draw_arrays = s_draw_arrays; sink.draw_index_array = s_draw_index;
    sink.inline_array = s_inline; sink.flip = s_flip; sink.immediate_vertex = s_immediate;

    rsx_dispatch* base = (rsx_dispatch*)malloc(sizeof *base);
    rsx_dispatch* rsx = (rsx_dispatch*)malloc(sizeof *rsx);
    if (!base || !rsx) { puts("FAIL: alloc"); return 1; }
    rsx_dispatch_init(base, &sink);
    /* Upload windows aim somewhere inside their arrays; a nonzero inline
     * stream so INLINE_ARRAY's cap branch is not the only one it has. */
    base->regs[0x1E9C >> 2] = 3u;     /* VP_UPLOAD_FROM_ID */
    base->regs[0x1EFC >> 2] = 5u;     /* VP_UPLOAD_CONST_ID */
    for (u32 i = 0; i < RSX_DSP_NUM_REGS; i++) base->seen[i] = 1u;   /* every register "written before" */

    static const u32 args[2] = { 0u, 0x80000001u };
    unsigned unsound = 0, plain = 0, conservative = 0;
    u32 first_unsound = 0xFFFFFFFFu;
    for (u32 idx = 0; idx < RSX_DSP_NUM_REGS; idx++) {
        const int mirror = rsx_dispatch_plain_register(idx);
        int behaved_plain = 1;
        for (int inside = 0; inside < 2 && behaved_plain; inside++) {
            for (int a = 0; a < 2 && behaved_plain; a++) {
                memcpy(rsx, base, sizeof *rsx);
                rsx->in_begin_end = inside;
                rsx->current_primitive = inside ? 5u : 0u;
                const u32 old_reg = rsx->regs[idx], old_seen = rsx->seen[idx];
                g_sink_calls = 0;
                rsx_dispatch_method(rsx, idx << 2, args[a]);
                int ok = g_sink_calls == 0 && rsx->regs[idx] == args[a] && rsx->seen[idx] == old_seen + 1u;
                /* Everything else must be as it was: put the two expected
                 * changes back and compare the whole dispatcher. */
                rsx->regs[idx] = old_reg; rsx->seen[idx] = old_seen;
                rsx->in_begin_end = base->in_begin_end; rsx->current_primitive = base->current_primitive;
                if (ok && memcmp(rsx, base, sizeof *rsx) != 0) ok = 0;
                if (!ok) behaved_plain = 0;
            }
        }
        if (mirror && !behaved_plain) { unsound++; if (first_unsound == 0xFFFFFFFFu) first_unsound = idx; }
        else if (mirror) plain++;
        else if (behaved_plain) conservative++;
    }
    printf("[%s] every register rsx_dispatch_plain_register calls plain is plain in every state "
           "(plain %u, special %u of which %u behave plainly but are kept special%s)\n",
           unsound ? "FAIL" : "PASS", plain, RSX_DSP_NUM_REGS - plain, conservative,
           unsound ? "" : "");
    if (unsound) printf("  %u unsound; first: method 0x%04X\n", unsound, first_unsound << 2);
    /* The hot registers of a draw must be plain, or the engine gains nothing. */
    int hot = rsx_dispatch_plain_register(0x1740 >> 2) && rsx_dispatch_plain_register(0x1680 >> 2) &&
              rsx_dispatch_plain_register(0x1A08 >> 2) && rsx_dispatch_plain_register(0x0A00 >> 2);
    printf("[%s] vertex formats, offsets, texture words and viewport are plain\n", hot ? "PASS" : "FAIL");
    free(base); free(rsx);
    return (unsound || !hot) ? 1 : 0;
}
