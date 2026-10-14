/* Synthetic committed VM/LS only. Real ordinary/list DMA functions. */
#include <stdint.h>
static uint64_t chronology_clock=1000000;
static uint64_t chronology_now(void) { return chronology_clock; }
#define AUDIO_CHRONOLOGY_TEST_CLOCK chronology_now
#ifdef AUDIO_CHRONOLOGY_PARENT_DMA
#include "../../diagnostics/audio_chronology_impl.h"
#endif
#define main existing_disposition_main
#include "test_spu_dma_disposition.c"
#undef main
#include <unistd.h>
static void advance(uint64_t us) { chronology_clock+=us; }
static void create_trigger(void) {
    const char* path=getenv("AUDIO_CHRONOLOGY_TRIGGER");assert(path);
    FILE* f=fopen(path,"w");assert(f);assert(!fclose(f));advance(250001);
}
static ac_record* last_kind(unsigned kind) {
    for(unsigned i=s_ac.count;i;i--)if(s_ac.records[i-1].kind==kind)return &s_ac.records[i-1];
    assert(0);return NULL;
}
static void consume(uint64_t index) {
    const uint8_t* p=memory+0x12000+(index%8)*2048;
    audio_chronology_consume(0,0,index,p,2048,index%8);
    advance(1);audio_chronology_consume(0,1,index,NULL,2048,index%8);
    memset((void*)p,0,2048);
    advance(1);audio_chronology_consume(0,2,index,NULL,2048,index%8);
    advance(1);audio_chronology_consume(0,3,index,NULL,2048,(index+1)%8);
}
static void* chronology_thread(void* arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<100;i++)audio_chronology_notify(id,0xABC,-1);
    return NULL;
}
int main(int argc,char** argv) {
    assert(argc==2);static spu_context ctx;static uint8_t ls[SPU_LS_SIZE];
    memset(g_vm_page_bitmap,255,sizeof g_vm_page_bitmap);ctx.ls=ls;ctx.spu_id=2;ctx.pc=0x1234;
    for(unsigned i=0;i<8;i++)spu_ls_write32(&ctx,0x100+i*4,0x3f800000u+i);
    errno=EDOM;audio_chronology_port(0,0x12000,2,8,1);assert(errno==EDOM);
    if(!strcmp(argv[1],"disabled") || !strcmp(argv[1],"pretrigger")) {
        audio_chronology_dma_token t;audio_chronology_dma_begin(&t,0x12000,32,0x20);
        assert(t.port==-1);audio_chronology_dma_after(&t,(const uint8_t*)1);
        audio_chronology_dma_end(&t,(const void*)1,0,0,0,0,0,1);
        audio_chronology_consume(0,0,0,(const uint8_t*)1,2048,0);
        assert(mfc_do_transfer(&ctx,0x100,0x12000,32,0x20)==0);
        assert(!memcmp(ls+0x100,memory+0x12000,32));assert(!s_ac.count);assert(errno==EDOM);
        return 0;
    }
    create_trigger();audio_chronology_poll();assert(s_ac.armed);assert(s_ac.count==1);
    if(!strcmp(argv[1],"threads")) {
        pthread_t ids[8];for(unsigned i=0;i<8;i++)assert(!pthread_create(&ids[i],NULL,chronology_thread,(void*)(uintptr_t)i));
        for(unsigned i=0;i<8;i++)assert(!pthread_join(ids[i],NULL));
        assert(s_ac.count==s_ac.limit);assert(s_ac.dropped==801-s_ac.limit);
        audio_chronology_finish();return 0;
    }
    if(!strcmp(argv[1],"cap")) {
        for(unsigned i=0;i<64;i++)audio_chronology_notify(3,42,-1);
        assert(s_ac.count==s_ac.limit);assert(s_ac.dropped==65-s_ac.limit);
        advance(10000001);audio_chronology_poll();assert(s_ac.flushed);
        unsigned count=s_ac.count;audio_chronology_notify(2,0,0);assert(s_ac.count==count);
        return 0;
    }
    if(!strcmp(argv[1],"epoch")) {
        audio_chronology_dma_token t;audio_chronology_dma_begin(&t,0x12000,32,0x20);assert(t.port==0);
        audio_chronology_port(0,0,0,0,0);audio_chronology_port(0,0x12000,2,8,1);
        audio_chronology_dma_after(&t,ls+0x100);audio_chronology_dma_end(&t,&ctx,2,0x1234,0x100,0x20,0,1);
        ac_record* r=last_kind(AC_DMA);assert((r->flags&3)==3);assert(r->epoch!=r->value);
        return 0;
    }
    if(!strcmp(argv[1],"overlap")) {
        audio_chronology_dma_token t;audio_chronology_dma_begin(&t,0x12000,32,0x20);
        consume(0);audio_chronology_dma_after(&t,ls+0x100);
        audio_chronology_dma_end(&t,&ctx,2,0x1234,0x100,0x20,0,1);
        assert(last_kind(AC_DMA)->flags&1);
        // Different slot consumption is not marked overlapping.
        audio_chronology_dma_begin(&t,0x12000,32,0x20);consume(1);
        audio_chronology_dma_after(&t,ls+0x100);audio_chronology_dma_end(&t,&ctx,2,0x1234,0x100,0x20,0,1);
        assert(!(last_kind(AC_DMA)->flags&1));return 0;
    }
    // Actual successful DMA publication before consume.
    advance(10);assert(!mfc_do_transfer(&ctx,0x100,0x12000,32,0x20));
    ac_record* dma=last_kind(AC_DMA);assert(dma->nz==8 && dma->mask==1 && (dma->flags&4));
    assert(dma->ctx==(uint64_t)(uintptr_t)&ctx && dma->ea==0x12000 && dma->lsa==0x100);
    advance(10);consume(0);ac_record* c=last_kind(AC_CONSUME);assert(c->nz==8 && dma->copy_us<c->us);
    // Actual zero publication remains distinguishable from no write.
    memset(ls+0x100,0,32);advance(10);assert(!mfc_do_transfer(&ctx,0x100,0x12800,32,0x20));
    assert(last_kind(AC_DMA)->nz==0 && (last_kind(AC_DMA)->flags&4));consume(1);
    // Late nonzero copy into the just-cleared slot, not auto-labelled underrun.
    spu_ls_write32(&ctx,0x100,0x3F800000);advance(10);assert(!mfc_do_transfer(&ctx,0x100,0x12000,32,0x20));
    assert(last_kind(AC_DMA)->nz==1 && last_kind(AC_DMA)->copy_us>last_kind(AC_CLEAR_END)->us);
    // Exact failed outcome, never certified successful by its tag/completion.
    assert(mfc_do_transfer(&ctx,SPU_LS_SIZE-16,0x12000,32,0x20)==-1);
    assert(last_kind(AC_DMA)->rc==-1 && !(last_kind(AC_DMA)->flags&4));
    // Two list elements retain their actual independent EA and LSA.
    spu_ls_write32(&ctx,0x400,16);spu_ls_write32(&ctx,0x404,0x13000);
    spu_ls_write32(&ctx,0x408,16);spu_ls_write32(&ctx,0x40C,0x13800);
    unsigned before=s_ac.count;assert(!mfc_run_list(&ctx,0x400,2,0x100,0,0x20,7));
    assert(s_ac.count==before+2);assert(s_ac.records[before].ea==0x13000 && s_ac.records[before+1].ea==0x13800);
    // Partial-byte copies do not falsely certify a whole zero float.
    assert(!mfc_do_transfer(&ctx,0x101,0x12001,1,0x20));
    assert(last_kind(AC_DMA)->words==0 && last_kind(AC_DMA)->size==1 && (last_kind(AC_DMA)->flags&4));
    // Slot-wrap identity has a monotonic index, not just ABA slot address.
    consume(8);assert(last_kind(AC_CONSUME)->index==8 && last_kind(AC_CONSUME)->ea==0x12000);
    audio_chronology_backend(0,256,0,256);advance(9000);audio_chronology_backend(1024,1280,0,256);
    assert(s_ac.zero_queue==1 && s_ac.min_queue==0 && s_ac.max_interval==9000);
    audio_chronology_notify(3,42,-1);assert(last_kind(AC_NOTIFY)->rc==-1);
    for(unsigned i=0;i<20;i++)audio_chronology_api(0,0,1,32,33,0);
    assert(s_ac.api_count[0]==16);
    for(unsigned i=0;i<20;i++)audio_chronology_api(1,0,32,33,1000,-1);
    assert(s_ac.api_count[1]==16);
    unsigned count=s_ac.count;advance(10000001);audio_chronology_poll();assert(s_ac.flushed);
    audio_chronology_consume(0,0,9,(const uint8_t*)1,2048,1);assert(s_ac.count==count);
    assert(errno==EDOM);puts("PASS actual-DMA chronology, zeros, failed/list outcomes, wrap, host queue and API bounds");
    return 0;
}
