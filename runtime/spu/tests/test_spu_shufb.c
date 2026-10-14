/* SHUFB architectural byte order and selector classes, on every host.
 * The expected values use word shifts, independently of SPU_W and SIMD code.
 * Synthetic vectors only; no title bytes. */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned cases;
static uint64_t digest = 1469598103934665603ull;
static uint32_t rng = 0x1A29B6D3u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static unsigned byte_at(u128 v, unsigned i) { return (v._u32[i / 4] >> (24 - (i % 4) * 8)) & 255u; }
static void put_byte(u128* v, unsigned i, unsigned b) {
    unsigned shift = 24 - (i % 4) * 8;
    v->_u32[i / 4] = (v->_u32[i / 4] & ~(255u << shift)) | ((b & 255u) << shift);
}
static u128 expected(u128 a, u128 b, u128 c) {
    u128 r = {};
    for (unsigned i = 0; i < 16; ++i) {
        unsigned s = byte_at(c, i), v;
        if (s >= 224) v = 128;
        else if (s >= 192) v = 255;
        else if (s >= 128) v = 0;
        else v = (s & 16) ? byte_at(b, s & 15) : byte_at(a, s & 15);
        put_byte(&r, i, v);
    }
    return r;
}
__attribute__((noinline)) u128 shufb_hot_wrapper(u128 a, u128 b, u128 c) { return spu_shufb(a, b, c); }
static void check(u128 a, u128 b, u128 c) {
    u128 want = expected(a, b, c), got = shufb_hot_wrapper(a, b, c);
    if (memcmp(&want, &got, 16)) { fprintf(stderr, "SHUFB mismatch case %u\n", cases); exit(1); }
    /* Lifted destination aliases each source/control register: read-before-write. */
    u128 aa = a, bb = b, cc = c;
    aa = spu_shufb(aa, b, c); bb = spu_shufb(a, bb, c); cc = spu_shufb(a, b, cc);
    if (memcmp(&want, &aa, 16) || memcmp(&want, &bb, 16) || memcmp(&want, &cc, 16)) {
        fprintf(stderr, "SHUFB alias mismatch case %u\n", cases); exit(1);
    }
    for (unsigned i = 0; i < 4; ++i) { digest ^= got._u32[i]; digest *= 1099511628211ull; }
    cases++;
}
static u128 uniform(unsigned value) { u128 c = {}; for (unsigned i=0;i<16;i++) put_byte(&c,i,value); return c; }
static void semantic_cases(void) {
    u128 a = {}, b = {}, c = {};
    for (unsigned i=0;i<16;i++) { put_byte(&a,i,i+1); put_byte(&b,i,i+65); put_byte(&c,i,i); }
    for (unsigned s=0;s<256;s++) {
        check(a,b,uniform(s));
        for (unsigned lane=0;lane<16;lane++) { u128 ctl=c;put_byte(&ctl,lane,s);check(a,b,ctl); }
    }
    /* Every bit of either 128-bit input, for every selector byte. */
    for (unsigned bit=0;bit<128;bit++) for (unsigned s=0;s<256;s++) {
        u128 v={},z={};v._u32[bit/32]=1u<<(bit%32);
        check(v,z,uniform(s));check(z,v,uniform(s));
    }
    /* All source-byte permutations: both tables and controls above 0x1f. */
    for (unsigned start=0;start<32;start++) for (unsigned stride=1;stride<32;stride+=2) {
        u128 ctl={};for(unsigned i=0;i<16;i++)put_byte(&ctl,i,(start+i*stride)&31);check(a,b,ctl);
    }
    for (unsigned it=0;it<200000;it++) {
        for (unsigned w=0;w<4;w++){a._u32[w]=next();b._u32[w]=next();c._u32[w]=next();}
        check(a,b,c);
        if (!(it%1000)) { check(a,a,c);check(a,b,a);check(a,b,b);check(c,c,c); }
    }
    check(uniform(0),uniform(255),uniform(0));
    check(uniform(0),uniform(255),uniform(31));
    check(uniform(0),uniform(255),uniform(128));
    check(uniform(0),uniform(255),uniform(192));
    check(uniform(0),uniform(255),uniform(224));
    printf("SHUFB PASS cases=%u digest=%016llx\n",cases,(unsigned long long)digest);
}
static double seconds(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9; }
static void bench(void) {
    u128 a[64],b[64],c[64],r={};const unsigned n=2000000;
    for(unsigned i=0;i<64;i++) for(unsigned w=0;w<4;w++){a[i]._u32[w]=next();b[i]._u32[w]=next();c[i]._u32[w]=next();}
    for(unsigned round=0;round<5;round++) {
        uint64_t sum=0;double t=seconds();
        for(unsigned i=0;i<n;i++){unsigned k=i&63;r=shufb_hot_wrapper(a[k],b[k],c[k]);sum+=r._u32[i&3];a[k]._u32[i&3]^=r._u32[(i+1)&3];}
        printf("SHUFB BENCH round=%u ns_per_op=%.3f checksum=%016llx\n",round,(seconds()-t)*1e9/n,(unsigned long long)sum);
    }
}
#ifdef SHUFB_INTERPRETER
#include "../spu_interp.h"
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c;(void)ch;abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v;abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch;abort(); }
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c,uint32_t a) { (void)c;(void)a;abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c;abort(); }
void spu_ls_watch_slow(uint32_t l,int w,const uint8_t*p,uint32_t pc,uint32_t lr) {
    (void)l;(void)w;(void)p;(void)pc;(void)lr;abort();
}
static void instruction(uint8_t* p,uint32_t value) {
    p[0]=value>>24;p[1]=value>>16;p[2]=value>>8;p[3]=value;
}
static void interpreted_aliases(void) {
    static spu_context ctx;
    for(unsigned selector=0;selector<256;selector++)for(unsigned destination=1;destination<=4;destination++) {
        memset(&ctx,0,sizeof ctx);spu_context_init(&ctx,0);ctx.image_id=-1;
        for(unsigned i=0;i<16;i++){put_byte(&ctx.gpr[1],i,i+1);put_byte(&ctx.gpr[2],i,i+65);}
        ctx.gpr[3]=uniform(selector);
        const u128 want=expected(ctx.gpr[1],ctx.gpr[2],ctx.gpr[3]);
        /* SHUFB RRR: rc=destination, ra=1, rb=2, rt=3 (control). */
        instruction(ctx.ls+0x100,(0xBu<<28)|(destination<<21)|(2u<<14)|(1u<<7)|3u);
        instruction(ctx.ls+0x104,0);
        spu_interp_run(&ctx,0x100);
        if(ctx.status!=SPU_STATUS_STOPPED_BY_STOP||memcmp(&want,&ctx.gpr[destination],16)) {
            fprintf(stderr,"SHUFB interpreted alias mismatch selector=%u destination=%u\n",selector,destination);exit(1);
        }
    }
    puts("SHUFB interpreter PASS cases=1024 (all selectors, distinct/A/B/control destinations)");
}
#endif
int main(int argc,char**argv) { if(argc>1&&!strcmp(argv[1],"--bench"))bench();else { semantic_cases();
#ifdef SHUFB_INTERPRETER
interpreted_aliases();
#endif
} return 0; }
