/* Real GCM object from the SDK library, real LIVE renderer, synthetic data. */
#define rsx_live_draw_report_batch_status fixture_real_report_status
#include "../rsx_live_draw.c"
#undef rsx_live_draw_report_batch_status
#include "live_draw_test_harness.h"
#include "../cellGcmSys.h"
#include "../gcm_checked_completion.h"
#define T "test_gcm_live_report_gpu"
extern uint32_t ppu_vm_size,ppu_hle_inject_base,g_ww_lo,g_ww_hi;
extern void cellGcm_native_consumer_init(void);
extern void cellGcm_syscall_set_fifo(u32,u32);
extern void cellGcm_syscall_iomap(u32,u32,u32);
extern u32 cellGcm_syscall_bringup(u32);
extern void cellGcm_rsx_process_fifo(void);
static unsigned status_positive;
static uint64_t last_validated_fence;
void ps3_ww_report_inline(unsigned int addr,unsigned long long value,int width){
    ldh_check("actual guest write follows physical completion",status_positive&&last_validated_fence&&g.fence&&g.fence->lpVtbl->GetCompletedValue(g.fence)>=last_validated_fence);
    printf("[ww-hle] %08x <- %llx width=%d\n",addr,value,width);
}
int rsx_live_draw_report_batch_status(uint64_t lifetime,uint64_t ticket,int wait){
    int status=fixture_real_report_status(lifetime,ticket,wait);
    if(status==1){
        ldh_check("actual status is backed by completed physical fence",g.fence&&g.fence->lpVtbl->GetCompletedValue(g.fence)>=ticket);
        status_positive++;last_validated_fence=ticket;printf("REAL_STATUS positive fence=%llu lifetime=%llu\n",(unsigned long long)ticket,(unsigned long long)lifetime);
    }
    return status;
}
static void raw(u32 ea,u32 value){value=_byteswap_ulong(value);memcpy(vm_base+ea,&value,4);}
static u32 peek(u32 ea){u32 value;memcpy(&value,vm_base+ea,4);return _byteswap_ulong(value);}
static int ready(void* user){(void)user;return rsx_live_draw_checked_ready();}
static int flush(void* user){(void)user;return rsx_live_draw_flush_checked();}
int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    const char* mode=argc>1?argv[1]:"batch";const int dependency=!strcmp(mode,"dependency"),partial=!strcmp(mode,"partial");
    _putenv("RSX_LIVE_DRAW=1");_putenv("PS3RECOMP_RSX_DEFERRED_REPORTS=1");_putenv("PS3RECOMP_GCM_LIVE_REPORT_BATCH=1");
    _putenv("PS3RECOMP_RSX_DEFERRED_REPORT_BATCH=32");_putenv("PS3RECOMP_GCM_CHECKED_REF=1");_putenv("MAG_NATIVE_CONSUMER=1");
    cellGcm_native_consumer_init();
    vm_base=VirtualAlloc(NULL,UINT64_C(0x100000000),MEM_RESERVE,PAGE_READWRITE);
    if(!vm_base||!VirtualAlloc(vm_base,0x4000000,MEM_COMMIT,PAGE_READWRITE))return 2;
    ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;
    if(cellGcmInit(0x10000,0x100000,0x100000)!=0||ldh_init(T))return 2;
    cellGcm_syscall_iomap(0x100000,0,0x100000);
    cellGcm_syscall_bringup(0);
    ShowWindow((HWND)rsx_null_backend_get_hwnd(),SW_HIDE);
    if(!cellGcm_set_checked_completion(NULL,ready,flush))return 2;
    unsigned records=partial?1:dependency?3:16,at=0;
    for(unsigned i=0;i<records;i++){
        raw(0x100000+at,0x00041800);raw(0x100004+at,0x01000000+i*16);at+=8;
        if(!partial){raw(0x100000+at,0x00081d6c);raw(0x100004+at,0x900+i*4);raw(0x100008+at,0x11223344+i);at+=12;}
    }
    if(dependency){raw(0x100000+at,0x00040050);raw(0x100004+at,0xcafebabe);at+=8;}
    cellGcm_syscall_set_fifo(at,0);
    ldh_setup_quad(0,1,1,0,0);rsx_test_fp_mov_r0_col0(ldh_main+0x1000,RSX_TEST_FP_SWZ_IDENT);
    ldh_surface(8|(1<<8),0x10000,32,0x200000,64,8,8);ldh_draw(0x1000,0x40,0,0,0x207);
    if(partial)ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
    const uint64_t before=g.fence_value;
    g_ww_lo=0x3000900;g_ww_hi=0x3000940;
    cellGcm_rsx_process_fifo();
    g_ww_lo=g_ww_hi=0;
    ldh_check("batch uses expected checked physical submissions",g.fence_value==before+(partial?0:dependency?2:1));
    ldh_check("status proof precedes publications",status_positive==(partial?0:records*2));
    if(partial){
        ldh_check("unfinished primitive report never publishes",cellGcmGetReport(1,0)==0);
        ldh_m(NV4097_SET_BEGIN_END,0);
    }else{
        uint64_t previous=0;
        for(unsigned i=0;i<records;i++){
            CellGcmReportData* report=cellGcmGetReportDataAddress(i);
            ldh_check("frozen report value and ordered timestamps",report&&report->value==0xffff&&report->timestamp>previous);
            if(report)previous=report->timestamp;
            u32 value=0x11223344+i;value=(value&0xff00ff00)|((value>>16)&0xff)|((value&0xff)<<16);
            ldh_check("ordered byte-swapped real guest label",peek(0x3000900+i*4)==value);
        }
        if(dependency)ldh_check("literal REF completion remains paced GCM output",peek(0x3002008)==0xcafebabe);
        u8 rgb[8*8*3];u32 w=0,h=0;int read=ldh_read_rgb(0x10000,"gcm-live.ppm",rgb,8,8,&w,&h);
        ldh_check("batched actual draw retains rendered pixel",!read&&w==8&&h==8&&rgb[(4*8+4)*3]>250);
    }
    rsx_live_draw_set_report_barrier(NULL);rsx_live_draw_set_report_lease(NULL,NULL);
    rsx_live_draw_shutdown();rsx_null_backend_shutdown();VirtualFree(vm_base,0,MEM_RELEASE);vm_base=NULL;
    return ldh_done(T);
}
