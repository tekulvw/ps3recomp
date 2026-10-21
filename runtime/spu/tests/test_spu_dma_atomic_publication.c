/* Real reservation operations and manufactured bytes only. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t guest[1u<<20];
uint8_t* vm_base=guest;
uint32_t ppu_vm_size;
/* Job I/O recorder (runtime/diagnostics): never armed here, so the fixture
 * only needs the inactive pointer and an event sink that is never reached. */
struct spu_context;
void* ps3_jobio_active;
void ps3_jobio_event(struct spu_context* c, uint32_t k, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t n, const uint8_t* b, uint32_t x)
{ (void)c; (void)k; (void)cmd; (void)lsa; (void)ea; (void)n; (void)b; (void)x; }
#include "../spu_channels.c"
static spu_context ctx;
static void* observer_thread(void* unused){
    (void)unused;
    for(unsigned i=0;i<32;i++)spu_dma_publication_observe_atomic(&ctx,0x2000,0x20000,0xD0,SPU_DMA_ATOMIC_GETLLAR_COMPLETE);
    return NULL;
}
static void be32(uint8_t* p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(int argc,char** argv){
    if(argc>1 && !strcmp(argv[1],"poison")) {
        spu_dma_publication_observe_atomic((const spu_context*)(uintptr_t)1,0,0x20000,0xD0,SPU_DMA_ATOMIC_GETLLAR_COMPLETE);
        spu_dma_publication_observe_atomic((const spu_context*)(uintptr_t)1,0,0x20000,0xB4,SPU_DMA_ATOMIC_PUTLLC_SUCCESS);
        return 0;
    }
    spu_context_init(&ctx,0);ctx.image_id=77;ctx.spu_id=3;
    if(argc>1 && !strcmp(argv[1],"bounds")) {
        ctx.ls=(uint8_t*)(uintptr_t)1;
        spu_dma_publication_observe_atomic(&ctx,SPU_LS_SIZE-127,0x20000,0xD0,SPU_DMA_ATOMIC_GETLLAR_COMPLETE);
        spu_dma_publication_observe_atomic(&ctx,0,0x100000000ull,0xB4,SPU_DMA_ATOMIC_PUTLLC_SUCCESS);
        spu_dma_publication_observe_atomic((const spu_context*)(uintptr_t)1,0,0x20000,0xD0,(enum spu_dma_publication_atomic_outcome)0);
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"threads")) {
        memset(ctx.ls+0x2000,0,128);ctx.ls[0x2004]=0x12;ctx.ls[0x2005]=0x34;ctx.ls[0x2006]=0x56;ctx.ls[0x2007]=0x78;
        pthread_t threads[8];
        for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,observer_thread,NULL));
        for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
        return 0;
    }
    uint32_t lsa=(argc>1 && !strcmp(argv[1],"end"))?SPU_LS_SIZE-128:0x2000;
    const uint32_t ea=0x20000;
    memset(guest+ea,0x5a,128);be32(guest+ea,0x00700001);be32(guest+ea+4,0x12345678);
    memset(ctx.ls+lsa,0xa5,128);ctx.mfc_lsa=lsa;ctx.mfc_eal=ea;
    ctx.pc=0x100;assert(spu_mfc_atomic(&ctx,MFC_GETLLAR_CMD));
    assert(ctx.atomic_stat==4 && ctx.resv_valid && !memcmp(ctx.ls+lsa,guest+ea,128));
    be32(ctx.ls+lsa,0x01000001);ctx.pc=0x104;
    assert(spu_mfc_atomic(&ctx,MFC_PUTLLC_CMD));
    assert(ctx.atomic_stat==0 && !ctx.resv_valid && !memcmp(ctx.ls+lsa,guest+ea,128));
    ctx.pc=0x108;assert(spu_mfc_atomic(&ctx,MFC_GETLLAR_CMD));
    assert(ctx.atomic_stat==4 && ctx.resv_valid);
    guest[ea+112]^=1; /* Controlled reservation comparison failure. */
    uint8_t before[128];memcpy(before,guest+ea,128);
    be32(ctx.ls+lsa,0x02000001);ctx.pc=0x10c;
    assert(spu_mfc_atomic(&ctx,MFC_PUTLLC_CMD));
    assert(ctx.atomic_stat==1 && !ctx.resv_valid && !memcmp(before,guest+ea,128));
    puts("PASS: real GETLLAR copy / PUTLLC success / failed reservation leaves guest bytes unchanged");
    return 0;
}

void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
