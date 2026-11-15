/* Differential check for the micro-interpreter's decoded-trace cache.
 * Generates random runtime "stubs" (straight runs of the microstep's
 * instruction set ending in an unconditional branch, some rewriting
 * themselves, some holding a conditional branch), runs each three times
 * from the same starting state -- the first builds the trace, the second
 * replays it, the third runs after the stub's words changed -- and prints a
 * digest of the full context and local store after every run. Run once with
 * PS3RECOMP_SPU_SMC_TRACE=0 and once with it on: the digests must match.
 * argv[1] = cases. Synthetic data only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size = 0;
#include "../spu_channels.c"
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
unsigned long long ps3_ms_now(void) { return 1000; }
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a; (void)b; }
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void ydkj_wake_all_event_flags(void) {}

static uint64_t s_rng;
static uint32_t rnd(void) { s_rng = s_rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s_rng >> 33); }
static void put(spu_context* c, uint32_t a, uint32_t w) { c->ls[a]=(uint8_t)(w>>24); c->ls[a+1]=(uint8_t)(w>>16); c->ls[a+2]=(uint8_t)(w>>8); c->ls[a+3]=(uint8_t)w; }
static uint32_t ri10(uint32_t op8, uint32_t i10, uint32_t ra, uint32_t rt) { return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt; }
static uint32_t ri16(uint32_t op9, uint32_t i16, uint32_t rt) { return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt; }
static uint32_t rr(uint32_t op11, uint32_t rb, uint32_t ra, uint32_t rt) { return (op11 << 21) | (rb << 14) | (ra << 7) | rt; }
static uint64_t digest(const spu_context* c)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = (const uint8_t*)c->gpr;
    for (size_t i = 0; i < sizeof c->gpr; i++) { h ^= p[i]; h *= 1099511628211ull; }
    for (size_t i = 0; i < SPU_LS_SIZE; i++) { h ^= c->ls[i]; h *= 1099511628211ull; }
    h ^= c->pc * 31u + (uint32_t)c->int_enable * 7u + (g_spu_trampoline_fn != 0); h *= 1099511628211ull;
    return h;
}
/* One random instruction from the microstep set; registers 2..9 are kept as
 * LS pointers so loads and stores stay near the stub. */
static uint32_t random_op(uint32_t pc, uint32_t stub, uint32_t len)
{
    const uint32_t rt = 10 + rnd() % 100, ra = 2 + rnd() % 8;
    switch (rnd() % 16) {
    case 0: case 1: case 2: return ri10(0x24, rnd(), ra, rt);                   /* stqd */
    case 3: case 4: return ri10(0x34, rnd(), ra, rt);                          /* lqd  */
    case 5: return ri16(0x041, (0x3F000 + 16 * (rnd() % 200)) >> 2, rt);       /* stqa */
    case 6: return ri16(0x061, (0x3F000 + 16 * (rnd() % 200)) >> 2, rt);       /* lqa  */
    case 7: return (rnd() % 4) ? ri16(0x047, rnd() % 64, rt)                   /* stqr */
                               : ri16(0x047, (uint32_t)((int32_t)(stub - pc) >> 2) + rnd() % (len + 1), rt); /* into the stub */
    case 8: return ri16(0x067, rnd() % 64, rt);                                /* lqr  */
    case 9: return ri10(0x44, rnd(), 10 + rnd() % 100, rt);                    /* xori */
    case 10: return ri10(0x1C, rnd(), 10 + rnd() % 100, rt);                   /* ai   */
    case 11: return rr(rnd() & 1 ? 0x040 : 0x0C0, 10 + rnd() % 100, 10 + rnd() % 100, rt); /* sf / a */
    case 12: return ri16(0x081 + (rnd() % 2), rnd(), rt);                      /* il / ilhu */
    case 13: return ri16(0x0C1, rnd(), rt);                                    /* iohl */
    case 14: return (0x21u << 25) | ((rnd() & 0x3FFFF) << 7) | rt;             /* ila  */
    default: { static const uint32_t nops[] = { 0x201, 0x001, 0x002, 0x003, 0x1AC };
               return rr(nops[rnd() % 5], 0, 0, 0); }
    }
}
static uint32_t terminator(spu_context* c, uint32_t pc)
{
    const uint32_t eb = (rnd() % 3 == 0) ? 0x40000u : (rnd() % 2 ? 0x80000u : 0);
    switch (rnd() % 7) {
    case 0: return rr(0x1A8, 0, 1, 0) | eb;                      /* bi $1 */
    case 1: return rr(0x1A9, 0, 1, 0) | eb;                      /* bisl $0,$1 */
    case 2: return rr(0x1AA, 0, 0, 0) | eb;                      /* iret */
    case 3: return ri16(0x064, rnd() % 1024, 0);                  /* br   */
    case 4: return ri16(0x060, rnd() % 1024, 0);                  /* bra  */
    case 5: return ri16(0x066, rnd() % 1024, 0);                  /* brsl */
    default: (void)c; (void)pc; return ri16(0x062, rnd() % 1024, 0); /* brasl */
    }
}
int main(int argc, char** argv)
{
    const unsigned cases = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    static spu_context init, c;
    spu_context_init(&c, 0);
    uint64_t all = 0;
    for (unsigned k = 0; k < cases; k++) {
        s_rng = 0x9E3779B97F4A7C15ull ^ k;
        const uint32_t len = 1 + rnd() % 200;
        const uint32_t stub = (k % 3 == 0) ? (0x3FEC0u - 0x400u) : (0x20000u + 4 * (rnd() % 0x1000));
        for (uint32_t i = 0; i < SPU_LS_SIZE; i += 4) put(&c, i, rnd());
        for (int r = 0; r < 128; r++) for (int w = 0; w < 4; w++) c.gpr[r]._u32[w] = rnd();
        for (int r = 2; r < 10; r++) c.gpr[r]._u32[0] = 0x30000u + 16 * (rnd() % 0x800);
        c.gpr[1]._u32[0] = 0x1000 + 4 * (rnd() % 1000); c.srr0 = 0x2000 + 4 * (rnd() % 1000);
        c.int_enable = rnd() & 1; c.image_id = 9;
        for (uint32_t i = 0; i < len; i++) put(&c, stub + 4 * i, random_op(stub + 4 * i, stub, len));
        if (k % 7 == 3) put(&c, stub + 4 * (rnd() % len), ri16(0x040 + 2 * (rnd() % 4), 1, 2 + rnd() % 8)); /* conditional inside */
        put(&c, stub + 4 * len, terminator(&c, stub + 4 * len));
        memcpy(&init, &c, sizeof c);
        uint8_t* ls0 = malloc(SPU_LS_SIZE); memcpy(ls0, c.ls, SPU_LS_SIZE);
        for (int run = 0; run < 3; run++) {
            memcpy(&c, &init, sizeof c); memcpy(c.ls, ls0, SPU_LS_SIZE);
            if (run == 2) put(&c, stub + 4 * (rnd() % len), random_op(stub, stub, len));   /* words changed */
            c.pc = stub; g_spu_trampoline_fn = 0;
            const int handled = spu_smc_microstep(&c);
            const uint64_t d = digest(&c) ^ (uint64_t)handled;
            all = all * 1099511628211ull ^ d;
            if (k < 4) printf("case %u run %d handled=%d digest=%016llx\n", k, run, handled, (unsigned long long)d);
        }
        free(ls0);
    }
    printf("cases=%u combined=%016llx\n", cases, (unsigned long long)all);
    return 0;
}
