/* Test the real decoder/step path: stale words, PC-relative targets, context
 * replacement and simultaneous host threads must not reuse the wrong decode.
 * The runner inserts a decode-entry counter into a temporary production copy. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instrumented-spu-interp.c"

u128 spu_rdch(spu_context* c,uint32_t x){(void)c;(void)x;abort();}
void spu_wrch(spu_context* c,uint32_t x,u128 v){(void)c;(void)x;(void)v;abort();}
uint32_t spu_rchcnt(spu_context* c,uint32_t x){(void)c;(void)x;abort();}
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c,uint32_t x){(void)c;(void)x;return 0;}
int spu_lifted_entry_maybe(uint32_t a) { (void)a; return 1; }
void spu_spurs_taskset_syscall(spu_context* c){(void)c;abort();}
void spu_ls_watch_slow(uint32_t a,int b,const uint8_t* c,uint32_t d,uint32_t e){(void)a;(void)b;(void)c;(void)d;(void)e;abort();}
static int cache_on;
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static void word(uint8_t* p,uint32_t w){p[0]=w>>24;p[1]=w>>16;p[2]=w>>8;p[3]=w;}
static int step(spu_context* c,uint32_t pc,uint32_t w){word(c->ls+pc,w);c->pc=pc;c->status=SPU_STATUS_RUNNING;return spu_step(c);}
static uint32_t il(unsigned rt,int n){return 0x40800000u|(((uint32_t)n&65535u)<<7)|rt;}
static void fresh(spu_context* c){memset(c,0,sizeof(*c));spu_context_init(c,0);c->image_id=-1;}
static void scalar(spu_context* c,unsigned r,uint32_t n){for(int k=0;k<4;k++)CHECK(c->gpr[r]._u32[k]==n);}
static uint64_t digest(uint64_t h,const void* data,size_t n){const uint8_t*p=data;for(size_t k=0;k<n;k++)h=(h^p[k])*1099511628211ull;return h;}
static void* thread_case(void* argument){
    uintptr_t n=(uintptr_t)argument;
    spu_context* c=malloc(sizeof(*c));assert(c);fresh(c);
    for(unsigned k=0;k<4096;k++){
        int r=step(c,0x140,il(7,(int)n));
        if(r||c->gpr[7]._u32[0]!=n)abort();
    }
    if(test_decode_calls!=(cache_on?1u:4096u))abort();
    free(c);return NULL;
}
int main(void){
    const char* e=getenv("SPU_INTERP_DECODE_CACHE");cache_on=!(e&&!strcmp(e,"0"));
    static spu_context a,b;fresh(&a);fresh(&b);
    CHECK(step(&a,0x100,il(2,5))==0);scalar(&a,2,5);
    unsigned long before=test_decode_calls;
    for(int k=0;k<16;k++){CHECK(step(&a,0x100,il(2,5))==0);scalar(&a,2,5);}
    CHECK(test_decode_calls-before==(cache_on?0u:16u));
    /* Real code replacement, including restoring an older overlay at same PC. */
    CHECK(step(&a,0x100,il(2,7))==0);scalar(&a,2,7);
    CHECK(step(&a,0x100,il(2,5))==0);scalar(&a,2,5);
    /* Same address reused by a different context with different live LS. */
    CHECK(step(&b,0x100,il(2,19))==0);scalar(&b,2,19);
    a.pc=0x100;CHECK(spu_step(&a)==0);scalar(&a,2,5);
    /* Relative branch +8: identical instruction bytes have different targets. */
    CHECK(step(&a,0x200,0x32000100u)==0);CHECK(a.pc==0x208);
    CHECK(step(&b,0x300,0x32000100u)==0);CHECK(b.pc==0x308);
    CHECK(step(&a,0x200,0x32000100u)==0);CHECK(a.pc==0x208);
    /* Last valid LS word: preserve inherited next-PC behavior, no new fetch. */
    CHECK(step(&a,0x3FFFC,il(3,-1))==0);CHECK(a.pc==0x40000);scalar(&a,3,UINT32_MAX);
    /* Zero is a valid first opcode, not an uninitialized-cache hit. */
    CHECK(step(&a,0,0)==1);CHECK(a.status==SPU_STATUS_STOPPED_BY_STOP);CHECK(a.pc==4);
    /* Actual STQD replaces all four code words through the production LS writer. */
    a.gpr[1]._u32[0]=0x100;
    for(unsigned k=0;k<4;k++)a.gpr[4]._u32[k]=il(2,23+(int)k);
    CHECK(step(&a,0x400,0x24000084u)==0);
    for(unsigned k=0;k<4;k++){a.pc=0x100+4*k;CHECK(spu_step(&a)==0);scalar(&a,2,23+k);}
    /* Full GPR + live LS + PC/status parity is compared between cached and
     * uncached executables. Also exercise branch immediates and RI10 forms. */
    uint64_t h=1469598103934665603ull;
    for(unsigned k=0;k<2048;k++){
        uint32_t pc=0x800+4*(k%257),op;
        switch(k%4){
        case 0:op=il(20+(k%64),(int)k-1024);break;
        case 1:op=0x1C000000u|((k&1023u)<<14)|(2u<<7)|3u;break;
        case 2:op=0x32000100u;break;
        default:op=0;break;
        }
        step(&a,pc,op);
        h=digest(h,a.gpr,sizeof(a.gpr));h=digest(h,a.ls,SPU_LS_SIZE);
        h=digest(h,&a.pc,sizeof(a.pc));h=digest(h,&a.status,sizeof(a.status));
    }
    /* Cached decode must equal the unchanged reference on arbitrary words,
     * including unsupported opcodes (decode only), different PCs and code
     * replacement. Do not compare padding in the decoder's result struct. */
    uint32_t random=0xDECAFBADu;
    for(unsigned k=0;k<8192;k++){
        random=random*1664525u+1013904223u;
        uint32_t raw=random,pc=(k*997u)&SPU_LS_MASK;
        /* Existing decoder shifts a signed negative LQD/STQD displacement;
         * exclude that inherited UB from this optimization's UBSan oracle. */
        if((raw>>24)==0x34u||(raw>>24)==0x24u)raw&=~(1u<<23);
        for(unsigned repeat=0;repeat<2;repeat++){
            spu_ins got,want;test_decode_path(raw,pc,&got);spu_decode1(raw,pc,&want);
            CHECK(got.op==want.op);CHECK(got.rt==want.rt);CHECK(got.ra==want.ra);
            CHECK(got.rb==want.rb);CHECK(got.rc==want.rc);CHECK(got.ch==want.ch);
            CHECK(got.imm==want.imm);CHECK(got.tgt==want.tgt);
        }
    }
    pthread_t ts[4];for(uintptr_t k=0;k<4;k++)CHECK(!pthread_create(&ts[k],NULL,thread_case,(void*)(k+31)));
    for(int k=0;k<4;k++)CHECK(!pthread_join(ts[k],NULL));
    printf("PASS checks=%u architecture=%016llx\n",checks,(unsigned long long)h);
    return 0;
}
