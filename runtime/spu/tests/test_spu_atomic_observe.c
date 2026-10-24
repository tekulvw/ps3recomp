/* Real channel/coherence equivalence: observer must not change commit, LR,
 * epochs or watches. Missing observer must fail at runtime, not at link. */
#define main unused_mfc_fixture_main
#include "test_spu_mfc_slots.c"
#undef main
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../../diagnostics/critical_trace.h"
#include "../spu_atomic_observe.h"
/* Baseline fallback permits a meaningful RED execution before API exists. */
__attribute__((weak)) int ps3_spu_atomic_observe_arm(uint64_t id,uint64_t n,const char* p){(void)id;(void)n;(void)p;return -1;}
__attribute__((weak)) int ps3_spu_atomic_observe_stop(uint64_t id){(void)id;return -1;}
__attribute__((weak)) int ps3_spu_atomic_observe_dump(uint64_t id){(void)id;return -1;}
static spu_context a,b;
static unsigned watches;
static void watched(void* p){(void)p;watches++;}
static void atomic_cmd(spu_context* c,uint32_t cmd,uint32_t ea)
{
    c->mfc_lsa=0x1000;c->mfc_eal=ea;c->mfc_eah=0;
    spu_wrch(c,MFC_Cmd,spu_make_preferred_u32(cmd));
}
static _Atomic int entered,release_watch;
static void delayed_watch(void* p)
{
    (void)p;atomic_store(&entered,1);while(!atomic_load(&release_watch))sched_yield();
}
static void* late_worker(void* p)
{
    (void)p;spu_context_init(&a,0);a.image_id=209;a.pc=0xC77C;a.spu_id=3;
    atomic_cmd(&a,MFC_PUTLLUC_CMD,0x20000);
    atomic_store(&entered,2);while(atomic_load(&release_watch)==1)sched_yield();
    atomic_cmd(&a,MFC_PUTLLUC_CMD,0x20100);atomic_store(&entered,3);
    while(atomic_load(&release_watch)==2)sched_yield();
    atomic_cmd(&a,MFC_GETLLAR_CMD,0x20100);spu_coh_unregister(&a);return NULL;
}
static spu_context cleanup_ctx[33];
static void* cleanup_worker(void* arg)
{
    spu_context* c=&cleanup_ctx[(unsigned)(uintptr_t)arg];spu_context_init(c,0);c->pc=0x2488;c->image_id=2;
    atomic_cmd(c,MFC_GETLLAR_CMD,0x20000);
    spu_coh_unregister(c);spu_coh_forget_range((uintptr_t)c,(uintptr_t)c+sizeof(*c));return NULL;
}
static void wait_for_file(const char* p)
{
    for(unsigned i=0;i<2000;i++){struct stat st;if(!stat(p,&st)&&st.st_size>0)return;usleep(1000);}
    CHECK(0);
}
static int special(const char* path,const char* which)
{
    spu_context_init(&a,0);a.image_id=209;a.pc=0xC77C;a.spu_id=3;
    if(!strcmp(which,"cleanup")){
        CHECK(ps3_spu_atomic_observe_arm(1,100,path)==0);
        for(unsigned i=0;i<33;i++){pthread_t t;CHECK(!pthread_create(&t,NULL,cleanup_worker,(void*)(uintptr_t)i));CHECK(!pthread_join(t,NULL));}
        CHECK(ps3_spu_atomic_observe_stop(1)==0);CHECK(ps3_spu_atomic_observe_dump(1)==0);
    }else if(!strcmp(which,"control")){
        char request[1200],legacy[1200];snprintf(request,sizeof(request),"%s.request",path);snprintf(legacy,sizeof(legacy),"%s.critical",path);
        setenv("PS3RECOMP_CRITICAL_TRACE_CONTROL",request,1);FILE* f=fopen(request,"w");CHECK(f!=NULL);
        fprintf(f,"spu-atomic 1 200 1000 %s\n",path);fclose(f);ps3_critical_control_start();
        for(unsigned i=0;i<2000&&!__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE);i++)usleep(1000);
        CHECK((__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE)&3u)==1);
        for(unsigned i=0;i<16;i++)atomic_cmd(&a,MFC_PUTLLUC_CMD,0x20000);
        for(unsigned i=0;i<2000&&(__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE)&3u)==1;i++)usleep(1000);
        atomic_cmd(&a,MFC_GETLLAR_CMD,0x20100);wait_for_file(path);CHECK(!ps3_critical_enabled());
        f=fopen(request,"w");CHECK(f!=NULL);fprintf(f,"2 5 %s\n",legacy);fclose(f);wait_for_file(legacy);
        for(unsigned i=0;i<100&&!ps3_critical_finalized_capture_id();i++)usleep(1000);
        CHECK(ps3_critical_finalized_capture_id()==2);unlink(request);spu_coh_unregister(&a);
    }else if(!strcmp(which,"sample")){
        spu_coh_watch w;spu_lockline_lock();spu_coh_watch_add(&w,0x20000,watched,NULL);spu_lockline_unlock();
        CHECK(ps3_spu_atomic_observe_arm(1,10000,path)==0);
        for(unsigned i=0;i<2048;i++)atomic_cmd(&a,MFC_PUTLLUC_CMD,0x20000);
        CHECK(watches==2048);CHECK(ps3_spu_atomic_observe_stop(1)==0);
        atomic_cmd(&a,MFC_GETLLAR_CMD,0x20100);CHECK(ps3_spu_atomic_observe_dump(1)==0);
        spu_lockline_lock();spu_coh_watch_remove(&w);spu_lockline_unlock();spu_coh_unregister(&a);
    }else if(!strcmp(which,"lateack")){
        spu_coh_watch w;spu_lockline_lock();spu_coh_watch_add(&w,0x20000,delayed_watch,NULL);spu_lockline_unlock();
        CHECK(ps3_spu_atomic_observe_arm(1,100,path)==0);pthread_t t;CHECK(!pthread_create(&t,NULL,late_worker,NULL));
        while(atomic_load(&entered)!=1)sched_yield();CHECK(ps3_spu_atomic_observe_stop(1)==0);
        atomic_store(&release_watch,1);while(atomic_load(&entered)!=2)sched_yield();
        atomic_store(&release_watch,3);CHECK(!pthread_join(t,NULL));CHECK(ps3_spu_atomic_observe_dump(1)==0);
        spu_lockline_lock();spu_coh_watch_remove(&w);spu_lockline_unlock();
    }else if(!strcmp(which,"overflow")||!strcmp(which,"budget")){
        CHECK(ps3_spu_atomic_observe_arm(1,!strcmp(which,"budget")?2:1000,path)==0);
        for(unsigned i=0;i<160;i++)atomic_cmd(&a,MFC_PUTLLUC_CMD,0x30000+128*i);
        CHECK(ps3_spu_atomic_observe_stop(1)==0);atomic_cmd(&a,MFC_GETLLAR_CMD,0x20100);
        CHECK(ps3_spu_atomic_observe_dump(1)==0);spu_coh_unregister(&a);
    }else if(!strcmp(which,"bounds")){
        char line[2048];
        snprintf(line,sizeof(line),"spu-atomic -1 1 1 %s",path);
        CHECK(ps3_spu_atomic_observe_control_request(line)==1);CHECK(access(path,F_OK)!=0);CHECK(!g_spu_atomic_observe_state);
        snprintf(line,sizeof(line),"spu-atomic 1 20001 1 %s",path);ps3_spu_atomic_observe_control_request(line);CHECK(access(path,F_OK)!=0);
        snprintf(line,sizeof(line),"spu-atomic 1 1 100000001 %s",path);ps3_spu_atomic_observe_control_request(line);CHECK(access(path,F_OK)!=0);
        snprintf(line,sizeof(line),"spu-atomic 1 1 1 %s extra",path);ps3_spu_atomic_observe_control_request(line);CHECK(access(path,F_OK)!=0);
        CHECK(ps3_spu_atomic_observe_arm(1,1,"relative")==-1);
        setenv("PS3_TPROF","1",1);CHECK(ps3_spu_atomic_observe_arm(1,1,path)==-1);CHECK(!g_spu_atomic_observe_state);unsetenv("PS3_TPROF");
        setenv("SPU_LOCKLINE_PROFILE","1",1);CHECK(ps3_spu_atomic_observe_arm(1,1,path)==-1);CHECK(!g_spu_atomic_observe_state);unsetenv("SPU_LOCKLINE_PROFILE");
        CHECK(ps3_spu_atomic_observe_arm(1,1,path)==0);CHECK(ps3_spu_atomic_observe_arm(2,1,path)==-1);
        CHECK(ps3_spu_atomic_observe_stop(1)==0);CHECK(ps3_spu_atomic_observe_stop(1)==-1);CHECK(ps3_spu_atomic_observe_dump(1)==0);
        CHECK(ps3_spu_atomic_observe_arm(1,1,path)==-1);CHECK(ps3_spu_atomic_observe_arm(2,1,path)==-1);CHECK(!g_spu_atomic_observe_state);
    }else if(!strcmp(which,"late")){
        spu_coh_watch w;spu_lockline_lock();spu_coh_watch_add(&w,0x20000,delayed_watch,NULL);spu_lockline_unlock();
        CHECK(ps3_spu_atomic_observe_arm(1,100,path)==0);pthread_t t;CHECK(!pthread_create(&t,NULL,late_worker,NULL));
        while(atomic_load(&entered)!=1)sched_yield();CHECK(ps3_spu_atomic_observe_stop(1)==0);CHECK(ps3_spu_atomic_observe_dump(1)==0);
        char next[1200];snprintf(next,sizeof(next),"%s.next",path);CHECK(ps3_spu_atomic_observe_arm(2,100,next)==0);
        atomic_store(&release_watch,1);while(atomic_load(&entered)!=2)sched_yield();
        atomic_store(&release_watch,2);while(atomic_load(&entered)!=3)sched_yield();
        CHECK(ps3_spu_atomic_observe_stop(2)==0);atomic_store(&release_watch,3);CHECK(!pthread_join(t,NULL));
        CHECK(ps3_spu_atomic_observe_dump(2)==0);spu_lockline_lock();spu_coh_watch_remove(&w);spu_lockline_unlock();
    }
    printf("OBSERVE_SPECIAL %s passed=%d failed=%d\n",which,g_pass,g_fail);return !!g_fail;
}
int main(int argc,char** argv)
{
    if(argc==4)return special(argv[1],argv[3]);
    CHECK(argc==3);if(argc!=3)return 1;
    int enabled=atoi(argv[2]);
    int armed=ps3_spu_atomic_observe_arm(1,100000,argv[1]);CHECK(armed==(enabled?0:-1));
    spu_context_init(&a,0);spu_context_init(&b,0);
    a.image_id=209;a.spu_id=3;a.pc=0xC77C;b.image_id=2;b.pc=0x2488;b.spu_id=4;
    spu_coh_watch watch;
    spu_lockline_lock();spu_coh_watch_add(&watch,0x20000,watched,NULL);
    uint64_t epoch=spu_coh_line_epoch(0x20000);spu_lockline_unlock();
    atomic_cmd(&a,MFC_GETLLAR_CMD,0x20017);CHECK(a.atomic_stat==4);
    atomic_cmd(&b,MFC_GETLLAR_CMD,0x20000);CHECK(b.atomic_stat==4);
    memset(a.ls+0x1000,0x53,128);
    atomic_cmd(&a,MFC_PUTLLC_CMD,0x20000);CHECK(a.atomic_stat==0);
    CHECK(!b.resv_valid && (b.event_status&SPU_EVENT_LR));
    CHECK(guest_memory[0x20000]==0x53 && watches==1);
    atomic_cmd(&b,MFC_PUTLLC_CMD,0x20000);CHECK(b.atomic_stat==1 && watches==1);
    memset(a.ls+0x1000,0x71,128);
    atomic_cmd(&a,MFC_PUTLLUC_CMD,0x20000);CHECK(a.atomic_stat==2 && watches==2);
    atomic_cmd(&a,MFC_PUTQLLUC_CMD,0x20000);CHECK(a.atomic_stat==2 && watches==3);
    spu_lockline_lock();CHECK(spu_coh_line_epoch(0x20000)==epoch+3);
    spu_coh_watch_remove(&watch);spu_lockline_unlock();
    if(enabled){CHECK(ps3_spu_atomic_observe_stop(1)==0);}
    /* Post-stop real command supplies safe boundary ACK; not counted. */
    atomic_cmd(&a,MFC_GETLLAR_CMD,0x20100);
    if(enabled){CHECK(ps3_spu_atomic_observe_dump(1)==0);}
    spu_coh_unregister(&a);spu_coh_unregister(&b);
    printf("OBSERVE_GUEST byte=%u watches=%u epoch_delta=3 stat=%u lr=%u\n",guest_memory[0x20000],watches,a.atomic_stat,(b.event_status&SPU_EVENT_LR)!=0);
    printf("OBSERVE_CHECKS passed=%d failed=%d\n",g_pass,g_fail);
    return !!g_fail;
}
