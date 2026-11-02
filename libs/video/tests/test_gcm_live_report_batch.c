/* Actual FIFO/queue with a synchronous completed-fence LIVE provider. */
#include <stdint.h>
uint64_t rsx_live_draw_report_batch_lifetime(void);
int rsx_live_draw_report_batch_admit(uint64_t);
uint64_t rsx_live_draw_report_batch_issue(uint64_t);
int rsx_live_draw_report_batch_status(uint64_t,uint64_t,int);
void rsx_live_draw_set_report_barrier(int(*)(void));
int rsx_live_draw_set_report_lease(void(*)(void),void(*)(void));
#define main checked_reference_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
static uint64_t life=7,fence;
static unsigned issues,statuses,admits;
static int partial,issue_bad,status_bad,change_life,inside_issue,reenter;
static int(*live_barrier)(void);
static unsigned effects;static u32 expected_effect;
static void effect(u32 method,u32 arg,u32 ea){
    (void)arg;(void)ea;if(method!=expected_effect)return;
    assert(issues==1&&!s_reports.count&&s_reports.published==1);effects++;
}
static void user_effect(u32 value){assert(value==123);effect(expected_effect,0,0);}
static void release_write(u32 ea,u64 value,int width){
    assert(ea==VM_HLE_INJECT_BASE+0x800&&value>=1&&value<=32&&width==4);
    assert(vm_read32(GCM_CONTROL_GUEST_ADDR+4)>=264);effects++;
}
uint64_t rsx_live_draw_report_batch_lifetime(void){return life;}
int rsx_live_draw_report_batch_admit(uint64_t expected){admits++;return expected&&expected==life&&!partial;}
uint64_t rsx_live_draw_report_batch_issue(uint64_t expected){
    assert(!inside_issue);inside_issue=1;issues++;
    assert(expected==life);assert(s_reports.count&&s_reports.unissued);
    if(reenter){cellGcm_rsx_consumer_begin();cellGcm_rsx_consumer_end();}
    if(held){pthread_mutex_lock(&hold_mutex);pending=1;pthread_cond_broadcast(&hold_cond);while(!released)pthread_cond_wait(&hold_cond,&hold_mutex);pthread_mutex_unlock(&hold_mutex);}
    if(change_life)life++;
    inside_issue=0;return issue_bad?0:++fence;
}
int rsx_live_draw_report_batch_status(uint64_t expected,uint64_t ticket,int wait){
    (void)wait;statuses++;return status_bad||expected!=life||!ticket||ticket>fence?-1:1;
}
void rsx_live_draw_set_report_barrier(int(*barrier)(void)){live_barrier=barrier;}
int rsx_live_draw_set_report_lease(void(*begin)(void),void(*end)(void)){return begin==cellGcm_rsx_consumer_begin&&end==cellGcm_rsx_consumer_end;}
static void report(unsigned index){gcm_get_report(0x01000000u+index*16u);}
static void* barrier_thread(void* arg){(void)arg;assert(live_barrier());return NULL;}
int main(int argc,char**argv){
    assert(argc==2);const char* mode=argv[1];int off=!strcmp(mode,"off"),legacy=!strcmp(mode,"legacy");
    setenv("PS3RECOMP_RSX_DEFERRED_REPORTS",off?"0":"1",1);
    setenv("PS3RECOMP_GCM_LIVE_REPORT_BATCH",off||legacy?"0":"1",1);
    setenv("PS3RECOMP_RSX_DEFERRED_REPORT_BATCH",!strcmp(mode,"batch1")?"1":"32",1);
    setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
    setenv("MAG_NATIVE_CONSUMER","1",1);cellGcm_native_consumer_init();
    assert(s_deferred_reports==!off);assert((live_barrier!=NULL)==(!off&&!legacy));
    if(off){assert(!issues&&!admits);puts("live report batching OFF passed");return 0;}
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(vm_base!=MAP_FAILED);
    ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;memset(s_ea_address_table,0xff,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1;s_ea_address_table[0xe0]=0x20;s_gcm_syscall_mode=1;s_report_dma=GCM_REPORT_DMA_LOCAL;
    external_backend=1;watched=0x3fff000;g_ww_lo=g_ww_hi=0;
    raw32(watched+12,0x10000);
    raw32(GCM_CONTROL_GUEST_ADDR+8,0xa5a5a5a5);assert(cellGcm_set_checked_completion(&external_backend,external_is_ready,external_flush));
    if(legacy){report(1);assert(!issues&&!admits&&checked_calls==1&&s_report_data[1].value==0xffff);
    }else if(!strcmp(mode,"held")){
        s_report_dma=GCM_REPORT_DMA_MAIN;memset(vm_base+0x2000010,0xa5,16);
        report(1);held=1;pthread_t thread;assert(!pthread_create(&thread,NULL,barrier_thread,NULL));
        pthread_mutex_lock(&hold_mutex);while(!pending)pthread_cond_wait(&hold_cond,&hold_mutex);
        assert(!s_reports.published&&!s_report_data[1].timestamp&&s_reports.count==1);
        for(unsigned i=0;i<16;i++)assert(vm_base[0x2000010+i]==0xa5);
        released=1;pthread_cond_broadcast(&hold_cond);pthread_mutex_unlock(&hold_mutex);
        assert(!pthread_join(thread,NULL));assert(s_reports.published==1&&!s_reports.count);
        assert(vm_read64(0x2000010)>0&&vm_read32(0x2000018)==0xffff&&vm_read32(0x200001c)==0);
    }else if(!strcmp(mode,"snapshot")||!strcmp(mode,"map-changed")){
        s_report_dma=GCM_REPORT_DMA_MAIN;memset(vm_base+0x2000010,0xa5,16);report(1);
        s_report_dma=GCM_REPORT_DMA_LOCAL;
        if(!strcmp(mode,"map-changed"))s_ea_address_table[0xe0]=0x21;
        if(!strcmp(mode,"map-changed")){
            assert(!live_barrier()&&s_reports.failed&&!s_reports.published);
            for(unsigned i=0;i<16;i++)assert(vm_base[0x2000010+i]==0xa5);
        }else{assert(live_barrier());assert(vm_read32(0x2000018)==0xffff&&!vm_read32(0x200001c));}
    }else if(!strcmp(mode,"swap")){
        report(1);assert(!cellGcm_set_checked_completion(NULL,NULL,NULL));assert(live_barrier());
        assert(cellGcm_set_checked_completion(NULL,NULL,NULL));
    }else if(!strcmp(mode,"pump")){
        report(1);ppu_gcm_pump();assert(!issues&&s_reports.count==1);
        GCM_PENDING_SET(1);ppu_gcm_pump();assert(issues==1&&!s_reports.count);
    }else if(!strcmp(mode,"frozen-config")){
        setenv("PS3RECOMP_RSX_DEFERRED_REPORTS","0",1);setenv("PS3RECOMP_GCM_LIVE_REPORT_BATCH","0",1);
        cellGcm_native_consumer_init();assert(s_deferred_reports&&s_live_report_batch&&live_barrier);
    }else if(!strcmp(mode,"partial")){
        partial=1;report(1);partial=0;assert(!issues&&s_reports.failed&&!s_reports.count);
    }else if(!strcmp(mode,"stale")){
        report(1);life++;assert(!live_barrier());assert(s_reports.failed&&!issues&&!s_reports.published);
    }else if(!strcmp(mode,"issue-failure")||!strcmp(mode,"status-failure")||!strcmp(mode,"life-during-issue")){
        issue_bad=!strcmp(mode,"issue-failure");status_bad=!strcmp(mode,"status-failure");change_life=!strcmp(mode,"life-during-issue");
        report(1);assert(!live_barrier());assert(s_reports.failed&&!s_reports.published&&s_reports.count==1);
    }else if(!strcmp(mode,"host-label")){
        raw32(0x100900,0xa5a5a5a5);raw32(VM_HLE_INJECT_BASE+0x900,0xa5a5a5a5);
        raw32(0x100000,0x000401a4);raw32(0x100004,0xfeed0001);
        raw32(0x100008,0x00081d6c);raw32(0x10000c,0x900);raw32(0x100010,0x11223344);
        cellGcm_syscall_set_fifo(20,0);cellGcm_rsx_process_fifo();
        assert(vm_read32(0x100900)==0x11443322&&vm_read32(VM_HLE_INJECT_BASE+0x900)==0xa5a5a5a5);
    }else if(!strcmp(mode,"release-get")){
        raw32(0x100000,0x00040064);raw32(0x100004,0x800);
        for(unsigned i=0;i<32;i++){raw32(0x100008+i*8,0x0004006c);raw32(0x10000c+i*8,i+1);}
        g_ww_lo=VM_HLE_INJECT_BASE+0x800;g_ww_hi=g_ww_lo+4;fixture_write_observer=release_write;
        cellGcm_syscall_set_fifo(264,0);cellGcm_rsx_process_fifo();assert(issues==1&&effects==32);
    }else if(!strncmp(mode,"effect-",7)){
        raw32(0x100000,0x00041800);raw32(0x100004,0x01000010);
        unsigned size=16;expected_effect=!strcmp(mode,"effect-ref")?0x50:!strcmp(mode,"effect-notify")?0x104:!strcmp(mode,"effect-flip")?0xe920:0xeb00;
        fixture_live_method_observer=effect;fixture_user_observer=user_effect;
        raw32(0x100008,0x00040000|expected_effect);raw32(0x10000c,expected_effect==0xeb00?123:0);
        if(!strcmp(mode,"effect-failure"))issue_bad=1;
        cellGcm_syscall_set_fifo(size,0);cellGcm_rsx_process_fifo();
        assert(issues==1&&effects==(issue_bad?0u:1u));
        if(issue_bad)assert(s_reports.failed&&!s_reports.published);
        if(expected_effect==0x50)assert(checked_calls==1&&s_checked_ref_receipt==1);
    }else if(!strcmp(mode,"inline")||!strcmp(mode,"vm")||!strcmp(mode,"timestamp")||!strcmp(mode,"pointer-read")){
        report(1);assert(!issues);
        if(!strcmp(mode,"inline"))assert(gcm_inline_image_barrier());
        else if(!strcmp(mode,"vm")){assert(gcm_report_vm_begin(0x2000,16));assert(s_consumer_depth==1);cellGcm_rsx_consumer_end();}
        else if(!strcmp(mode,"timestamp"))assert(cellGcmGetTimeStamp(1)>0);
        else assert(cellGcmGetReportDataAddress(1)->value==0xffff);
        assert(issues==1&&!s_reports.count&&s_reports.published==1);
    }else if(!strcmp(mode,"fifo-mixed")){
        unsigned at=0;
        for(unsigned i=0;i<16;i++){
            raw32(0x100000+at,0x00041800);raw32(0x100004+at,0x01000000+i*16);at+=8;
            raw32(0x100000+at,0x00081d6c);raw32(0x100004+at,0x900+i*4);raw32(0x100008+at,0x11223344+i);at+=12;
        }
        cellGcm_syscall_set_fifo(at,0);cellGcm_rsx_process_fifo();
        assert(issues==1&&s_reports.published==32&&!s_reports.count);
        for(unsigned i=0;i<16;i++){assert(s_report_data[i].value==0xffff);u32 v=0x11223344+i;v=(v&0xff00ff00)|((v>>16)&0xff)|((v&0xff)<<16);assert(vm_read32(VM_HLE_INJECT_BASE+0x900+i*4)==v);}
    }else if(!strcmp(mode,"read")){
        report(1);assert(!issues);assert(cellGcmGetReport(1,1)==0xffff);assert(issues==1&&!s_reports.count);
    }else if(!strcmp(mode,"idle")){
        report(1);assert(!issues);cellGcm_rsx_process_fifo();assert(issues==1&&s_reports.published==1&&!s_reports.count);
    }else if(!strcmp(mode,"capacity")){
        s_reports.batch=RSX_DEFERRED_REPORT_CAPACITY;
        for(unsigned i=0;i<RSX_DEFERRED_REPORT_CAPACITY;i++)report(i);
        assert(issues==1&&s_reports.published==RSX_DEFERRED_REPORT_CAPACITY&&!s_reports.count);
    }else{
        reenter=!strcmp(mode,"reentrant");unsigned n=!strcmp(mode,"batch1")?1:32;
        cellGcm_rsx_consumer_begin();for(unsigned i=0;i<n;i++)report(i);cellGcm_rsx_consumer_end();
        assert(issues==1&&s_reports.published==n&&!s_reports.count&&admits==n);
        for(unsigned i=0;i<n;i++){assert(s_report_data[i].value==0xffff&&s_report_data[i].timestamp);if(i)assert(s_report_data[i].timestamp>s_report_data[i-1].timestamp);}
    }
    assert(!inside_issue);munmap(vm_base,0x100000000ULL);puts("live report batch scenario passed");return 0;
}
