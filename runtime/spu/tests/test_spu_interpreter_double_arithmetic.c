/* Real encoded DFS/DFA/DFM instructions must process both doubleword lanes.
 * Literal IEEE-754 vector expectations catch missing dispatch, operand reversal,
 * lane/word ordering, source clobbering and destination-alias bugs. */
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPU-only instructions never reach these external channel/diagnostic hooks. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
int spu_lifted_entry_maybe(uint32_t a) { (void)a; return 1; }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}


static void be32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static spu_context ctx;
static void reset(void) {
    memset(&ctx, 0, sizeof(ctx));
    spu_context_init(&ctx, 0);
    ctx.image_id = -1;
}
static void check(const char* name, unsigned reg, const uint32_t want[4]) {
    if (memcmp(ctx.gpr[reg]._u32, want, 16)) {
        fprintf(stderr, "%s r%u: %08X:%08X:%08X:%08X expected %08X:%08X:%08X:%08X\n",
                name, reg, ctx.gpr[reg]._u32[0], ctx.gpr[reg]._u32[1],
                ctx.gpr[reg]._u32[2], ctx.gpr[reg]._u32[3],
                want[0], want[1], want[2], want[3]);
        abort();
    }
}
static void run(const char* name, uint32_t opcode, const uint32_t a[4],
                const uint32_t b[4], const uint32_t want[4], int alias) {
    unsigned ra = (opcode >> 7) & 127, rb = (opcode >> 14) & 127;
    unsigned rt = alias == 1 ? ra : alias == 2 ? rb : opcode & 127;
    reset();
    memcpy(ctx.gpr[ra]._u32, a, 16);
    memcpy(ctx.gpr[rb]._u32, b, 16);
    const uint32_t sentinel[4] = {0x11223344,0x55667788,0x99AABBCC,0xDDEEFF00};
    memcpy(ctx.gpr[100]._u32, sentinel, 16);
    be32(ctx.ls + 0x100, (opcode & ~127u) | rt);
    be32(ctx.ls + 0x104, 0);
    spu_interp_run(&ctx, 0x100);
    if (ctx.status != SPU_STATUS_STOPPED_BY_STOP || ctx.pc != 0x108) {
        fprintf(stderr, "%s alias=%d stopped at %X status=%d\n", name, alias, ctx.pc, ctx.status);
        abort();
    }
    check(name, rt, want);
    if (rt != ra) check(name, ra, a);
    if (rt != rb) check(name, rb, b);
    check(name, 100, sentinel);
}

static void audio_sequence(void) {
    reset();
    /* Exact instructions from original audio LS12698..126CC, omitting the
     * unrelated subroutine/LS loads. Synthetic finite inputs use exact fractions. */
    static const uint32_t program[] = {
        0x770019B0, /* fesd r48,r51 */
        0x77001931, /* fesd r49,r50 */
        0x59AC582F, /* dfs r47,r48,r49 */
        0x598C582D, /* dfa r45,r48,r49 */
        0x598B9784, /* dfa r4,r47,r46 */
        0x598B9683, /* dfa r3,r45,r46 */
        0x770028AC, /* fesd r44,r81 */
        0x59CB01AB, /* dfm r43,r3,r44 */
        0x772015A9, /* frds r41,r43 */
        0
    };
    const uint32_t a[4] = {0x3FC00000,0xDEADBEEF,0xC0100000,0x12345678}; /* 1.5,-2.25 */
    const uint32_t b[4] = {0x3F000000,0x89ABCDEF,0x40800000,0x87654321}; /* .5,4 */
    const uint32_t factor[4] = {0x40000000,0xAABBCCDD,0xBF000000,0x11223344}; /* 2,-.5 */
    const uint32_t offset[4] = {0x3FF00000,0,0x3FE00000,0}; /* 1,.5 */
    memcpy(ctx.gpr[51]._u32,a,16); memcpy(ctx.gpr[50]._u32,b,16);
    memcpy(ctx.gpr[81]._u32,factor,16); memcpy(ctx.gpr[46]._u32,offset,16);
    for (unsigned i=0;i<sizeof(program)/sizeof(program[0]);i++) be32(ctx.ls+0x100+4*i,program[i]);
    spu_interp_run(&ctx,0x100);
    assert(ctx.status==SPU_STATUS_STOPPED_BY_STOP && ctx.pc==0x128);
    const uint32_t difference[4]={0x3FF00000,0,0xC0190000,0}; /* 1,-6.25 */
    const uint32_t sum[4]={0x40000000,0,0x3FFC0000,0}; /* 2,1.75 */
    const uint32_t product[4]={0x40180000,0,0xBFF20000,0}; /* 6,-1.125 */
    const uint32_t singles[4]={0x40C00000,0,0xBF900000,0};
    check("audio DFS",47,difference); check("audio DFA",45,sum);
    check("audio DFM",43,product); check("audio FRDS",41,singles);
    check("audio source",51,a); check("audio source",50,b);
    check("audio source",81,factor); check("audio source",46,offset);
}
int main(void) {
    /* Each exact retail opcode uses its original source and destination registers. */
    static const struct {
        const char* name; uint32_t opcode, a[4], b[4], want[4];
    } cases[] = {
        {"DFS fractions",0x59AC582F,{0x3FF80000,0,0xC0020000,0},{0x3FE00000,0,0x40100000,0},{0x3FF00000,0,0xC0190000,0}},
        {"DFA fractions",0x598C582D,{0x3FF80000,0,0xC0020000,0},{0x3FE00000,0,0x40100000,0},{0x40000000,0,0x3FFC0000,0}},
        {"DFM fractions",0x59CB01AB,{0x3FF80000,0,0xC0020000,0},{0x3FE00000,0,0x40100000,0},{0x3FE80000,0,0xC0220000,0}},
        {"DFS mantissa",0x59AC582F,{0x3FF00000,1,0x40000000,2},{0x3FE00000,0,0x3FF00000,0},{0x3FE00000,2,0x3FF00000,4}},
        {"DFA mantissa",0x598C582D,{0x3FF00000,1,0x40000000,2},{0x3FE00000,0,0x3FF00000,0},{0x3FF80000,1,0x40080000,2}},
        {"DFM mantissa",0x59CB01AB,{0x3FF00000,1,0x40000000,2},{0x40000000,0,0x3FE00000,0},{0x40000000,1,0x3FF00000,2}},
        {"DFS signed zero",0x59AC582F,{0,0,0x80000000,0},{0x80000000,0,0,0},{0,0,0x80000000,0}},
        {"DFA signed zero",0x598C582D,{0x80000000,0,0,0},{0x80000000,0,0x80000000,0},{0x80000000,0,0,0}},
        {"DFM signed zero",0x59CB01AB,{0x80000000,0,0,0},{0xC0000000,0,0xC0080000,0},{0,0,0x80000000,0}},
        {"DFS cancellation",0x59AC582F,{0xBFE80000,0,0x3FD00000,0},{0xBFE80000,0,0x3FD00000,0},{0,0,0,0}},
        {"DFA cancellation",0x598C582D,{0xBFE80000,0,0x3FD00000,0},{0x3FE80000,0,0xBFD00000,0},{0,0,0,0}},
        {"DFM signs",0x59CB01AB,{0xBFE80000,0,0xBFD00000,0},{0xBFE00000,0,0x40000000,0},{0x3FD80000,0,0xBFE00000,0}},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
        for (int alias=0;alias<3;alias++)
            run(cases[i].name,cases[i].opcode,cases[i].a,cases[i].b,cases[i].want,alias);
    audio_sequence();
    puts("PASS: 36 encoded double arithmetic vectors and original audio conversion sequence");
    return 0;
}
