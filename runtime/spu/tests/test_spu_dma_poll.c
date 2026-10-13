/* A repeated MFC GET can wait on an asynchronous producer; it is not a halt.
 * Real channel submit, tag completion, VM coherency and execution landing pad. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint8_t guest[1u << 20];
uint8_t* vm_base = guest;
uint32_t ppu_vm_size;
#include "../spu_channels.c"
static spu_context consumer;
static unsigned polls, cancelled, produced;
static uint32_t command;
static const uint32_t source_ea=0x22000, target_lsa=0x1800;
static void delay(void) {
    const struct timespec pause={0,100000};
    nanosleep(&pause,NULL);
}
static void* publish_after_polling(void* unused) {
    (void)unused;
    /* A count handshake, rather than a scheduling-dependent sleep, ensures
     * the producer publishes only after the old 256-repeat heuristic fires. */
    while(__atomic_load_n(&polls,__ATOMIC_ACQUIRE)<1024) {
        if(__atomic_load_n(&cancelled,__ATOMIC_ACQUIRE))return NULL;
        delay();
    }
    spu_lockline_lock();
    guest[source_ea]=0xDE;guest[source_ea+1]=0xAD;
    guest[source_ea+2]=0xBE;guest[source_ea+3]=0xEF;
    spu_coh_notify_write(source_ea);
    spu_lockline_unlock();
    __atomic_store_n(&produced,1,__ATOMIC_RELEASE);
    return NULL;
}
static void wait_for_producer(spu_context* ctx) {
    for(unsigned n=0;n<4096;n++) {
        spu_wrch(ctx,MFC_LSA,spu_splat_u32(target_lsa));
        spu_wrch(ctx,MFC_EAL,spu_splat_u32(source_ea));
        spu_wrch(ctx,MFC_EAH,spu_splat_u32(0));
        spu_wrch(ctx,MFC_Size,spu_splat_u32(4));
        spu_wrch(ctx,MFC_TagID,spu_splat_u32(5));
        spu_wrch(ctx,MFC_Cmd,spu_splat_u32(command));
        spu_wrch(ctx,MFC_WrTagMask,spu_splat_u32(1u<<5));
        spu_wrch(ctx,MFC_WrTagUpdate,spu_splat_u32(2));
        assert(spu_rdch(ctx,MFC_RdTagStat)._u32[0]==(1u<<5));
        unsigned completed=__atomic_add_fetch(&polls,1,__ATOMIC_RELEASE);
        if(spu_ls_read32(ctx,target_lsa)==0xDEADBEEF) {
            ctx->gpr[10]=spu_splat_u32(completed);
            return;
        }
        delay();
    }
    fputs("FAIL: delayed producer was not observed within fixture bound\n",stderr);
    abort();
}
int main(int argc,char** argv) {
    command=(argc>1 && !strcmp(argv[1],"getf"))?MFC_GETF_CMD:MFC_GET_CMD;
    spu_context_init(&consumer,0);consumer.image_id=-1;
    consumer.pc=0x1000;consumer.status=SPU_STATUS_RUNNING;
    pthread_t producer;assert(!pthread_create(&producer,NULL,publish_after_polling,NULL));
    int halted=spu_run_with_halt(wait_for_producer,&consumer);
    __atomic_store_n(&cancelled,1,__ATOMIC_RELEASE);
    assert(!pthread_join(producer,NULL));
    printf("cmd=%02X halted=%d polls=%u produced=%u word=%08X status=%X\n",
           command,halted,polls,produced,spu_ls_read32(&consumer,target_lsa),consumer.status);
    fflush(stdout);
    assert(halted==0);
    assert(polls>=1024 && produced && consumer.gpr[10]._u32[0]>=1024);
    assert(consumer.status==SPU_STATUS_RUNNING);
    assert(spu_ls_read32(&consumer,target_lsa)==0xDEADBEEF);
    spu_mfc_release(&consumer);
    puts("PASS: repeated GET observes delayed producer without a synthetic guest halt");
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
