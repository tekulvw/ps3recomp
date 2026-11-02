/* Synthetic guest bytes change through the actual NV308A COLOR FIFO path. */
#define main unused_report_gpu_main
#define ps3_ww_report_inline unused_report_write_watch
#include "test_gcm_live_report_gpu.c"
#undef ps3_ww_report_inline
#undef main

static unsigned inline_writes;
void ps3_ww_report_inline(unsigned int address,unsigned long long value,int width)
{
    (void)address;(void)value;
    ldh_check("inline mutation follows pending report physical completion",
        width==4 && status_positive && last_validated_fence &&
        g.fence->lpVtbl->GetCompletedValue(g.fence)>=last_validated_fence);
    inline_writes++;
}
static void command(u32* at,u32 sub,u32 method,u32 value)
{raw(0x100000+*at,0x00040000u|(sub<<13)|method);raw(0x100004+*at,value);*at+=8;}
static u32 bits(float value){u32 result;memcpy(&result,&value,4);return result;}
static void fp_color(float r,float green)
{
    rsx_test_fp_instr(ldh_main+0x1000,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,
        2u|RSX_TEST_FP_SWZ_IDENT);
    const float color[4]={r,green,0,1};
    for(u32 i=0;i<4;i++)rsx_test_fp_put_word(ldh_main+0x1010+i*4,bits(color[i]));
}
static void draw(int indexed)
{
    if(!indexed){ldh_draw(0x1000,0x40,0,0,0x207);return;}
    ldh_m(NV4097_SET_DEPTH_TEST_ENABLE,0);ldh_m(NV4097_SET_DEPTH_MASK,0);
    ldh_m(NV4097_SET_SHADER_CONTROL,0x40);ldh_m(NV4097_SET_SHADER_PROGRAM,0x1002);
    ldh_m(0x181c,0x4000);ldh_m(0x1820,1); /* main-memory u32 indices */
    ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
    ldh_m(0x1824,3u<<24);ldh_m(NV4097_SET_BEGIN_END,0);
}
static HRESULT STDMETHODCALLTYPE reject_close(ID3D12GraphicsCommandList* list)
{(void)list;return E_FAIL;}
int main(int argc,char**argv)
{
    const char* mode=argc>1?argv[1]:"fp";
    const int vertex=!strcmp(mode,"vertex"),indexed=!strcmp(mode,"index");
    const int pending=!strcmp(mode,"pending"),failure=!strcmp(mode,"failure");
    const int partial=!strcmp(mode,"partial"),off=!strcmp(mode,"off"),batchoff=!strcmp(mode,"batch-off");
    const int pressure=!strcmp(mode,"pressure");
    const int reference=!strcmp(mode,"reference");
    _putenv("RSX_LIVE_DRAW=1");_putenv("PS3RECOMP_RSX_DEFERRED_REPORTS=1");
    _putenv(batchoff?"PS3RECOMP_GCM_LIVE_REPORT_BATCH=0":"PS3RECOMP_GCM_LIVE_REPORT_BATCH=1");
    _putenv(off?"PS3RECOMP_GCM_INLINE_NO_GPU_WAIT=0":"PS3RECOMP_GCM_INLINE_NO_GPU_WAIT=1");
    _putenv("PS3RECOMP_RSX_DEFERRED_REPORT_BATCH=32");_putenv("PS3RECOMP_GCM_CHECKED_REF=1");
    _putenv("MAG_NATIVE_CONSUMER=1");cellGcm_native_consumer_init();
    vm_base=VirtualAlloc(NULL,UINT64_C(0x100000000),MEM_RESERVE,PAGE_READWRITE);
    if(!vm_base||!VirtualAlloc(vm_base,0x4000000,MEM_COMMIT,PAGE_READWRITE))return 2;
    ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;
    if(cellGcmInit(0x10000,0x100000,0x100000)||ldh_init("test_gcm_live_inline_gpu"))return 2;
    free(ldh_main);ldh_main=vm_base+0x200000;
    cellGcm_syscall_iomap(0x100000,0,0x100000);
    cellGcm_syscall_iomap(0x200000,0x100000,0x100000);cellGcm_syscall_bringup(0);
    ShowWindow((HWND)rsx_null_backend_get_hwnd(),SW_HIDE);
    if(!cellGcm_set_checked_completion(NULL,ready,flush))return 2;
    ldh_setup_quad(0,1,1,0,0);
    if(vertex||indexed){
        rsx_test_fp_mov_r0_col0(ldh_main+0x1000,RSX_TEST_FP_SWZ_IDENT);
        for(u32 i=0;i<4;i++){ldh_wf(i*48+16,1);ldh_wf(i*48+20,0);ldh_wf(i*48+24,0);}
    }else fp_color(1,0);
    if(indexed){
        memcpy(ldh_main+192,ldh_main,192);
        for(u32 i=0;i<4;i++){ldh_wf((i+4)*48+16,0);ldh_wf((i+4)*48+20,1);ldh_w32(0x4000+i*4,i);}
    }
    ldh_surface(8|(1<<8),0x10000,32,0x200000,64,8,8);
    u32 at=0;if(pending||failure)command(&at,0,0x1800,0x01000000);
    const u32 destination=vertex?0u:indexed?0x4000u:0x1010u;
    command(&at,2,0x188,0xfeed0001);command(&at,2,0x304,256u<<16);
    command(&at,2,0x30c,0x100000+destination);command(&at,4,0x304,0);
    const u32 count=vertex?48u:4u;
    raw(0x100000+at,(count<<18)|(4u<<13)|0x400u);at+=4;
    for(u32 i=0;i<count;i++) {
        u32 value;
        if(vertex){memcpy(&value,ldh_main+i*4,4);value=_byteswap_ulong(value);
            if(i%12==4)value=0;if(i%12==5)value=bits(1);}
        else if(indexed)value=i+4;
        else {const float c[4]={0,1,0,1};value=bits(c[i]);value=(value<<16)|(value>>16);}
        raw(0x100000+at,value);at+=4;
    }
    if(reference)command(&at,0,0x50,0xcafebabe);
    /* FIFO reconfiguration has its own genuine lifetime barrier. Complete it
     * before either draw so the measured boundary is only the inline path. */
    cellGcm_syscall_set_fifo(at,0);
    ldh_m(NV4097_SET_SCISSOR_HORIZONTAL,4u<<16);draw(indexed);
    if(partial)ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
    const u64 before=g.fence_value;
    const ID3D12GraphicsCommandListVtbl* original=g.list->lpVtbl;ID3D12GraphicsCommandListVtbl proxy=*original;
    if(failure){proxy.Close=reject_close;g.list->lpVtbl=&proxy;}
    if(pending){g_ww_lo=0x200000+destination;g_ww_hi=g_ww_lo+count*4;}
    cellGcm_rsx_process_fifo();g_ww_lo=g_ww_hi=0;
    if(failure){
        ldh_check("failed pending-report completion suppresses inline mutation",!g.ready&&!status_positive&&peek(0x201010)==0x00003f80);
        g.list->lpVtbl=original;
    }else{
        const u64 expected=pending||partial||off||batchoff||reference?1:0;
        ldh_check("inline has only required genuine submissions",g.fence_value==before+expected);
        if(reference)ldh_check("literal REF still requires genuine completion and publication",peek(0x3002008)==0xcafebabe);
        if(pending)ldh_check("pending report publishes before all inline words",status_positive==1&&inline_writes==4);
        if(partial)ldh_m(NV4097_SET_BEGIN_END,0);
        if(pressure)g.cb_used=CB_RING_BYTES;
        ldh_m(NV4097_SET_SCISSOR_HORIZONTAL,(4u<<16)|4u);draw(indexed);
        if(pressure)ldh_check("constant-ring pressure retains original completion before reuse",g.fence_value==before+1);
        u8 rgb[8*8*3];u32 w=0,h=0;const int read=!ldh_read_rgb(0x10000,"inline-snapshots.ppm",rgb,8,8,&w,&h);
        const u8* left=rgb+(4*8+1)*3;const u8* right=rgb+(4*8+6)*3;
        ldh_check("old and new draws retain distinct immutable snapshots",read&&w==8&&h==8&&
            left[0]>250&&left[1]<3&&left[2]<3&&right[0]<3&&right[1]>250&&right[2]<3);
    }
    rsx_live_draw_set_report_barrier(NULL);rsx_live_draw_set_report_lease(NULL,NULL);
    rsx_live_draw_shutdown();rsx_null_backend_shutdown();VirtualFree(vm_base,0,MEM_RELEASE);vm_base=NULL;
    return ldh_done("test_gcm_live_inline_gpu");
}
