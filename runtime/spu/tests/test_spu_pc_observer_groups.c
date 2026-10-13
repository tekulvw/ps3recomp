/* Synthetic overlay groups: guards/caps must precede dynamic reads. */
#include "../spu_pc_observer.h"
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
static spu_context contexts[8];
static const uint32_t opcode=0x40200000;
static void setup(spu_context* c,unsigned group) {
    memset(c,0,sizeof *c); spu_context_init(c,0);
    for(unsigned i=0;i<3;i++) spu_ls_write32(c,0x30000+i*4,opcode);
    spu_ls_write32(c,0x30010,0xABCDEF00);
    spu_ls_write32(c,0x30100,0xAAA0+group);
    c->gpr[3]._u32[0]=c->gpr[5]._u32[0]=0x1000;
    c->gpr[4]._u32[0]=16;c->gpr[6]._u32[0]=32;
    c->gpr[7]._u32[0]=0xD000+group;
    for(unsigned i=0;i<4;i++)spu_ls_write32(c,0x1000+i*4,0x3F800000+i*0x800000);
}
static void event(spu_context* c,unsigned group) {spu_pc_observe(c,0x30000+(group-1)*4,opcode);}
static void* worker(void* p) {
    unsigned k=(unsigned)(uintptr_t)p;spu_context* c=&contexts[k];setup(c,1);
    spu_context before=*c;uint8_t ls[SPU_LS_SIZE];memcpy(ls,c->ls,sizeof ls);
    for(unsigned pass=0;pass<30;pass++)for(unsigned g=1;g<=3;g++) {
        spu_ls_write32(c,0x30100,0xAAA0+g);event(c,g);
    }
    spu_ls_write32(c,0x30100,0xAAA1);
    assert(!memcmp(&before,c,sizeof *c)&&!memcmp(ls,c->ls,sizeof ls));
    return NULL;
}
int main(int argc,char** argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"invalid")) {
        spu_pc_observe((spu_context*)1,0x30000,opcode);
        assert(ReadAcquire(&g_spu_pc_observer.gate)==0);return 0;
    }
    /* Parent observer must fail here because group config is unsupported. */
    assert(spu_pc_observer_enabled());
    if(!strcmp(argv[1],"parse")) {
        spu_pc_observe((spu_context*)1,0x3FFFF,0);
        assert(!ReadAcquire(&g_spu_pc_observer.events));return 0;
    }
    if(!strcmp(argv[1],"threads")) {
        pthread_t th[8];for(unsigned k=0;k<8;k++)assert(!pthread_create(&th[k],NULL,worker,(void*)(uintptr_t)k));
        for(unsigned k=0;k<8;k++)assert(!pthread_join(th[k],NULL));
        assert(ReadAcquire(&g_spu_pc_observer.events)==12);return 0;
    }
    spu_context* c=&contexts[0];setup(c,1);
    if(!strcmp(argv[1],"guards")) {
        /* Inaccessible GPRs with a readable LS pointer prove both guard sets
         * precede even the dynamic register reads. */
        size_t page=(size_t)sysconf(_SC_PAGESIZE),off=offsetof(spu_context,ls),shift=(page-off%page)%page;
        size_t span=((sizeof *c+shift+page-1)/page)*page;
        assert(shift+sizeof c->gpr<=page);
        void* m=mmap(NULL,span,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(m!=MAP_FAILED);
        spu_context* protected_c=(spu_context*)((uint8_t*)m+shift);protected_c->ls=c->ls;
        assert(!mprotect(m,page,PROT_NONE));
        spu_ls_write32(c,0x30100,0xAAA2);event(protected_c,1);
        spu_ls_write32(c,0x30100,0xAAA1);spu_ls_write32(c,0x30010,0);event(protected_c,1);
        assert(ReadAcquire(&g_spu_pc_observer.events)==0);
        assert(!munmap(m,span));spu_ls_write32(c,0x30010,0xABCDEF00);
        c->gpr[3]._u32[0]=SPU_LS_SIZE-8;event(c,1);
        assert(ReadAcquire(&g_spu_pc_observer.events)==0);
        c->gpr[3]._u32[0]=0x1000;event(c,1);
        assert(ReadAcquire(&g_spu_pc_observer.events)==1);return 0;
    }
    if(!strcmp(argv[1],"alias")) {
        for(unsigned g=1;g<=2;g++) {
            spu_ls_write32(c,0x30100,0xAAA0+g);
            for(unsigned i=0;i<6;i++)event(c,1);
        }
        assert(ReadAcquire(&g_spu_pc_observer.events)==4);return 0;
    }
    for(unsigned g=1;g<=3;g++) {
        spu_ls_write32(c,0x30100,0xAAA0+g);
        uint8_t ls[SPU_LS_SIZE];memcpy(ls,c->ls,sizeof ls);spu_context before=*c;
        for(unsigned i=0;i<20;i++)event(c,g);
        assert(ReadAcquire(&g_spu_pc_observer.events)==(LONG)(g*4));
        assert(!memcmp(ls,c->ls,sizeof ls)&&!memcmp(&before,c,sizeof *c));
        if(g==1) {
            int observe=1;
            /* Its unique site is capped, but later groups retain budget. */
            spu_pc_observe_cached((spu_context*)1,0x30000,opcode,&observe);assert(observe);
            event((spu_context*)1,1);
        }
    }
    int observe=1;spu_pc_observe_cached((spu_context*)1,0x30000,opcode,&observe);assert(!observe);
    return 0;
}
