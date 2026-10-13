/* Header-only C/C++ safety boundary and initialization contention fixture. */
#include "../spu_pc_observer.h"
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
static spu_context c;
#ifdef SPU_OBSERVER_LINKAGE
extern void spu_pc_observer_linkage_check(spu_pc_observer_state*);
#endif
static void* poison(void* unused) {
    (void)unused;
    for(unsigned i=0;i<10000;i++) spu_pc_observe((spu_context*)1,0x30000,0x34000194);
    return NULL;
}
int main(int argc,char** argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"poison")) {
        pthread_t threads[8];
        for(unsigned i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,poison,NULL));
        for(unsigned i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
        assert(ReadAcquire(&g_spu_pc_observer.gate)==0);
        clock_t begin=clock();
        for(unsigned i=0;i<10000000;i++) spu_pc_observe((spu_context*)1,0x30000,0x34000194);
        printf("disabled 10M observer calls: %.6fs\n",(double)(clock()-begin)/CLOCKS_PER_SEC);
        return 0;
    }
    if(!strcmp(argv[1],"nonmatch")) {
        spu_pc_observe((spu_context*)1,0x30001,0x34000194);
        spu_pc_observe((spu_context*)1,0x30000,0);
        assert(ReadAcquire(&g_spu_pc_observer.gate)==1);
        return 0;
    }
    spu_context_init(&c,0);
    spu_ls_write32(&c,0x30000,0x34000194);
    spu_ls_write32(&c,0x30010,0x35002780);
    c.gpr[3]._u32[0]=c.gpr[5]._u32[0]=0x1000;
    c.gpr[4]._u32[0]=16; c.gpr[6]._u32[0]=32;
    if(!strcmp(argv[1],"cap")) {
        assert(spu_pc_observer_enabled());
        for(unsigned i=0;i<g_spu_pc_observer.limit;i++) spu_pc_observe(&c,0x30000,0x34000194);
        int observe=1;
        spu_pc_observe_cached((spu_context*)1,0x30000,0x34000194,&observe);
        assert(!observe);
        spu_pc_observe((spu_context*)1,0x30000,0x34000194);
        assert((uint32_t)ReadAcquire(&g_spu_pc_observer.events)==g_spu_pc_observer.limit);
    } else if(!strcmp(argv[1],"guard")) {
        /* Matched opcode but wrong overlay guard: reject before pointer reads. */
        spu_ls_write32(&c,0x30010,0);
        c.gpr[3]._u32[0]=0xFFFFFFFF;
        spu_pc_observe(&c,0x30000,0x34000194);
        /* Place gpr in an inaccessible page and the ls pointer in the next:
         * mismatched code must return before even reading dynamic registers. */
        size_t page=(size_t)sysconf(_SC_PAGESIZE), ls_offset=offsetof(spu_context,ls);
        size_t shift=(page-ls_offset%page)%page;
        size_t span=((sizeof(spu_context)+shift+page-1)/page)*page;
        assert(shift+sizeof(c.gpr)<=page);
        void* mapping=mmap(NULL,span,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        assert(mapping!=MAP_FAILED);
        spu_context* guarded=(spu_context*)((uint8_t*)mapping+shift);
        guarded->ls=c.ls;
        assert(!mprotect(mapping,page,PROT_NONE));
        spu_pc_observe(guarded,0x30000,0x34000194);
        assert(!munmap(mapping,span));
        assert(!ReadAcquire(&g_spu_pc_observer.events));
    } else if(!strcmp(argv[1],"bounds")) {
        const uint32_t pointers[]={SPU_LS_SIZE+1,0xFFFFFFFF,SPU_LS_SIZE-8};
        for(unsigned i=0;i<3;i++) {
            c.gpr[3]._u32[0]=pointers[i]; spu_pc_observe(&c,0x30000,0x34000194);
            c.gpr[3]._u32[0]=0x1000;
            c.gpr[5]._u32[0]=pointers[i]; spu_pc_observe(&c,0x30000,0x34000194);
            c.gpr[5]._u32[0]=0x1000;
        }
        c.gpr[4]._u32[0]=16385; spu_pc_observe(&c,0x30000,0x34000194);
        c.gpr[4]._u32[0]=16; c.gpr[6]._u32[0]=3; spu_pc_observe(&c,0x30000,0x34000194);
        assert(!ReadAcquire(&g_spu_pc_observer.events));
    } else if(!strcmp(argv[1],"stats")) {
        const uint32_t values[]={0x80000000,0x00000001,0x7F800000,0x7FC00001};
        for(unsigned i=0;i<4;i++) spu_ls_write32(&c,0x1000+i*4,values[i]);
        spu_pc_observe(&c,0x30000,0x34000194);
        /* Six bytes means one whole f32: don't read the trailing half word. */
        c.gpr[4]._u32[0]=6; c.gpr[6]._u32[0]=12;
        spu_pc_observe(&c,0x30000,0x34000194);
        /* The last valid four bytes and an empty one-past span are legal. */
        c.gpr[3]._u32[0]=SPU_LS_SIZE-4; c.gpr[4]._u32[0]=4;
        c.gpr[5]._u32[0]=SPU_LS_SIZE; c.gpr[6]._u32[0]=0;
        spu_ls_write32(&c,SPU_LS_SIZE-4,0xBF800000);
        spu_pc_observe(&c,0x30000,0x34000194);
        assert(ReadAcquire(&g_spu_pc_observer.events)==3);
        assert(spu_ls_read32(&c,0x1000)==0x80000000);
        assert(spu_ls_read32(&c,0x1004)==0x00000001);
        assert(spu_ls_read32(&c,0x1008)==0x7F800000);
        assert(spu_ls_read32(&c,0x100C)==0x7FC00001);
    } else abort();
#ifdef SPU_OBSERVER_LINKAGE
    spu_pc_observer_linkage_check(&g_spu_pc_observer);
#endif
    return 0;
}
