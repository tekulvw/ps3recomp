/* Observe the real platform file lookup to verify throttling and cached path.
 * The wrapper delegates every lookup; it does not fake time or file state. */
#define GetFileAttributesA observed_file_attributes
#include "../spu_pc_observer.h"
#undef GetFileAttributesA
#ifdef __cplusplus
extern "C" {
#endif
DWORD GetFileAttributesA(const char*);
#ifdef __cplusplus
}
#endif
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static volatile LONG polls;
DWORD observed_file_attributes(const char* path) {
    InterlockedIncrement(&polls);
    return GetFileAttributesA(path);
}
static spu_context c, before;
static uint8_t expected[SPU_LS_SIZE];
static void observe(void) { spu_pc_observe(&c,0x30000,0x34000194); }
int main(int argc,char** argv) {
    assert(argc==4);
    const char* trigger=argv[2];
    c.ls=(uint8_t*)mmap(NULL,SPU_LS_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(c.ls!=MAP_FAILED);
    c.gpr[3]._u32[0]=c.gpr[5]._u32[0]=0x1000;
    c.gpr[4]._u32[0]=16; c.gpr[6]._u32[0]=32;
    spu_ls_write32(&c,0x30000,0x34000194);
    spu_ls_write32(&c,0x30010,0x35002780);
    for(unsigned i=0;i<4;i++) spu_ls_write32(&c,0x1000+i*4,0x3F800000+i*0x800000);
    assert(spu_pc_observer_enabled());
    if(!strcmp(argv[1],"reject")) {
        FILE* f=fopen(trigger,"wb"); assert(f); assert(!fclose(f));
        spu_pc_observe((spu_context*)1,0x30001,0x34000194); /* no site */
        spu_ls_write32(&c,0x30010,0); c.gpr[3]._u32[0]=0xFFFFFFFF;
        observe(); /* guard rejects before dynamic pointers or trigger IO */
        spu_ls_write32(&c,0x30010,0x35002780); observe(); /* invalid span */
        assert(ReadAcquire(&polls)==0 && ReadAcquire(&g_spu_pc_observer.events)==0);
        c.gpr[3]._u32[0]=0x1000;
        observe(); assert(ReadAcquire(&polls)==1 && ReadAcquire(&g_spu_pc_observer.events)==1);
    } else {
        memcpy(expected,c.ls,sizeof expected); memcpy(&before,&c,sizeof c);
        assert(!mprotect(c.ls,(size_t)sysconf(_SC_PAGESIZE),PROT_NONE));
        SetLastError(1234); errno=ERANGE;
        ULONGLONG start=GetTickCount64();
        for(unsigned i=0;i<10000;i++) observe();
        ULONGLONG elapsed=GetTickCount64()-start;
        assert(ReadAcquire(&g_spu_pc_observer.events)==0);
        assert(GetLastError()==1234 && errno==ERANGE);
        assert(ReadAcquire(&polls)>=1 && (ULONGLONG)ReadAcquire(&polls)<=elapsed/250+2);
        /* Caller-owned config/environment changes cannot alter the cached path. */
        FILE* f=fopen(argv[3],"wb"); assert(f); fputs("invalid\n",f); assert(!fclose(f));
        assert(!setenv("SPU_PC_OBSERVE","/nonexistent/config",1));
        assert(!mkdir(trigger,0700)); usleep(300000); observe();
        assert(ReadAcquire(&g_spu_pc_observer.events)==0); /* directory is not a file */
        assert(!rmdir(trigger));
        f=fopen(trigger,"wb"); assert(f); assert(!fclose(f));
        /* Restore the source before presence may arm it; a host scheduling
         * pause must not make this real-clock fixture dereference PROT_NONE. */
        assert(!mprotect(c.ls,(size_t)sysconf(_SC_PAGESIZE),PROT_READ|PROT_WRITE));
        usleep(300000);
        observe(); assert(ReadAcquire(&g_spu_pc_observer.events)==1);
        LONG armed_polls=ReadAcquire(&polls);
        assert(!unlink(trigger));
        observe(); observe(); /* latch persists after owner removes file */
        assert(ReadAcquire(&g_spu_pc_observer.events)==3 && ReadAcquire(&polls)==armed_polls);
        spu_pc_observe((spu_context*)1,0x30000,0x34000194); /* exhausted cap */
        assert(!memcmp(expected,c.ls,sizeof expected) && !memcmp(&before,&c,sizeof c));
    }
    assert(!munmap(c.ls,SPU_LS_SIZE));
    return 0;
}
