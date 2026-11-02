/* Synthetic real-D3D completion and whole-texture staging boundaries. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD WINAPI report_test_wait(HANDLE, DWORD);
#define WaitForSingleObject report_test_wait
#include "../rsx_live_draw.c"
#undef WaitForSingleObject
#include "live_draw_test_harness.h"

static int lease_depth, lease_begins, lease_ends, barrier_calls, deny_barrier;
static void lease_begin(void)
{
    ldh_check("host lease acquired before renderer lock", TryAcquireSRWLockExclusive(&g_ld_access_lock));
    ReleaseSRWLockExclusive(&g_ld_access_lock);
    ++lease_depth; ++lease_begins;
}
static void lease_end(void)
{
    ldh_check("lease surrounds complete visible effect", lease_depth == 1 &&
        (deny_barrier || !g.ready || g.display_buffers[0].resolve_src == 1));
    --lease_depth; ++lease_ends;
}
static int report_barrier(void)
{
    ++barrier_calls;
    ldh_check("barrier runs inside host lease", lease_depth == 1);
    if (!deny_barrier && g.ready) {
        const u64 t = rsx_live_draw_report_batch_issue(rsx_live_draw_report_batch_lifetime());
        ldh_check("issue does not recurse through host barrier", t != 0);
    }
    return !deny_barrier;
}

static void check_upload_pixels(ID3D12Resource* texture, int cube)
{
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width=(cube?6u:1u)*3072u;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;
    rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb=NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&rb))) {
        ldh_check("readback allocated",0);return;
    }
    D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=texture;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&b);
    for(u32 face=0;face<(cube?6u:1u);face++)for(u32 mip=0;mip<2;mip++) {
        D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};
        src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=face*2+mip;
        dst.pResource=rb;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset=face*3072u+(mip?2048u:0u);
        dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width=dst.PlacedFootprint.Footprint.Height=mip?4:8;
        dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=256;
        g.list->lpVtbl->CopyTextureRegion(g.list,&dst,0,0,0,&src,NULL);
    }
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&b);
    if(!rsx_live_draw_flush_checked()){ldh_check("readback genuine completion",0);return;}
    const u8* bytes=NULL;D3D12_RANGE range={0,(SIZE_T)rd.Width};
    int ok=SUCCEEDED(rb->lpVtbl->Map(rb,0,&range,(void**)&bytes));
    if(ok)for(u32 face=0;face<(cube?6u:1u);face++)for(u32 mip=0;mip<2;mip++)
        for(u32 y=0;y<(mip?4u:8u);y++)for(u32 x=0;x<(mip?16u:32u);x++)
            if(bytes[face*3072u+(mip?2048u:0u)+y*256u+x]!=(mip?0x97u:0x35u))ok=0;
    ldh_check("every face and mip has the actual uploaded bytes",ok);
    if(bytes){D3D12_RANGE none={0,0};rb->lpVtbl->Unmap(rb,0,&none);}
    rb->lpVtbl->Release(rb);
}

static void upload_pressure(int cube)
{
    u8 first[8 * 8 * 4], second[4 * 4 * 4];
    memset(first, 0x35, sizeof first); memset(second, 0x97, sizeof second);
    tex_level_t levels[12];
    for (u32 face = 0; face < (cube ? 6u : 1u); face++) {
        levels[face * 2] = (tex_level_t){8,8,first,32,8};
        levels[face * 2 + 1] = (tex_level_t){4,4,second,16,4};
    }
    /* Prior commands own the arena until the original checked fence. */
    g.upload_used = UPLOAD_SIZE - 256;
    const u64 before = g.fence_value;
    ID3D12Resource* texture = cube
        ? create_texture_cube(DXGI_FORMAT_R8G8B8A8_UNORM, levels, 2)
        : create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, levels, 2);
    ldh_check("whole texture pressure submits the original list before reuse",
        texture && g.ready && g.fence_value == before + 1 &&
        g.upload_used == (cube ? 6u : 1u) * 3072u);
    if (texture) {
        check_upload_pixels(texture,cube);
        /* Its copy commands still own it until actual completion. */
        retire_texture(texture);
        ldh_check("uploaded texture retires after genuine completion", rsx_live_draw_flush_checked());
    }
}

static const char* failure_mode;
static const ID3D12GraphicsCommandListVtbl* real_list;
static const ID3D12CommandAllocatorVtbl* real_allocator;
static const ID3D12CommandQueueVtbl* real_queue;
static const ID3D12FenceVtbl* real_fence;
static const ID3D12DeviceVtbl* real_device;
static ID3D12GraphicsCommandListVtbl list_proxy;
static ID3D12CommandAllocatorVtbl allocator_proxy;
static ID3D12CommandQueueVtbl queue_proxy;
static ID3D12FenceVtbl fence_proxy;
static ID3D12DeviceVtbl device_proxy;
static unsigned closes,executes,signals,allocator_resets,list_resets;
static unsigned checked_waits;
static unsigned recording_after_failure;
static void STDMETHODCALLTYPE count_pipeline(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso)
{
    if(!g.ready)++recording_after_failure;
    real_list->SetPipelineState(list,pso);
}
static DWORD WINAPI report_test_wait(HANDLE event,DWORD timeout)
{
    ++checked_waits;
    return failure_mode && !strcmp(failure_mode,"wait") ? WAIT_TIMEOUT : WaitForSingleObject(event,timeout);
}
static HRESULT STDMETHODCALLTYPE fail_close(ID3D12GraphicsCommandList* p)
{++closes;return !strcmp(failure_mode,"close")?E_FAIL:real_list->Close(p);}
static HRESULT STDMETHODCALLTYPE fail_list_reset(ID3D12GraphicsCommandList* p,ID3D12CommandAllocator* a,ID3D12PipelineState* s)
{++list_resets;return !strcmp(failure_mode,"list-reset")?E_FAIL:real_list->Reset(p,a,s);}
static HRESULT STDMETHODCALLTYPE fail_allocator_reset(ID3D12CommandAllocator* p)
{++allocator_resets;return !strcmp(failure_mode,"reset")?E_FAIL:real_allocator->Reset(p);}
static void STDMETHODCALLTYPE count_execute(ID3D12CommandQueue* p,UINT n,ID3D12CommandList*const* lists)
{++executes;real_queue->ExecuteCommandLists(p,n,lists);}
static HRESULT STDMETHODCALLTYPE fail_signal(ID3D12CommandQueue* p,ID3D12Fence* f,UINT64 v)
{++signals;return !strcmp(failure_mode,"signal")?E_FAIL:real_queue->Signal(p,f,v);}
static UINT64 STDMETHODCALLTYPE fail_completed(ID3D12Fence* p)
{return !strcmp(failure_mode,"fence")?UINT64_MAX:
    (!strcmp(failure_mode,"incomplete")||!strcmp(failure_mode,"wait"))?0:real_fence->GetCompletedValue(p);}
static HRESULT STDMETHODCALLTYPE fail_device(ID3D12Device* p)
{return !strcmp(failure_mode,"device")?DXGI_ERROR_DEVICE_REMOVED:real_device->GetDeviceRemovedReason(p);}
static void failure_case(const char* mode)
{
    failure_mode=mode;real_list=g.list->lpVtbl;real_allocator=g.alloc->lpVtbl;
    real_queue=g.queue->lpVtbl;real_fence=g.fence->lpVtbl;real_device=g.dev->lpVtbl;
    list_proxy=*real_list;list_proxy.Close=fail_close;list_proxy.Reset=fail_list_reset;
    allocator_proxy=*real_allocator;allocator_proxy.Reset=fail_allocator_reset;
    queue_proxy=*real_queue;queue_proxy.ExecuteCommandLists=count_execute;queue_proxy.Signal=fail_signal;
    fence_proxy=*real_fence;fence_proxy.GetCompletedValue=fail_completed;
    device_proxy=*real_device;device_proxy.GetDeviceRemovedReason=fail_device;
    const u64 lifetime=rsx_live_draw_report_batch_lifetime();
    const u8 value[4]={7,9,11,255};ID3D12Resource* texture=create_texture_rgba(value,1,1);
    ldh_check("failure fixture records real upload",texture && retire_texture(texture));
    g.list->lpVtbl=&list_proxy;g.alloc->lpVtbl=&allocator_proxy;g.queue->lpVtbl=&queue_proxy;
    g.fence->lpVtbl=&fence_proxy;g.dev->lpVtbl=&device_proxy;
    ldh_check("failed original operation never returns a completion ticket",!rsx_live_draw_report_batch_issue(lifetime) && !g.ready);
    ldh_check("failed renderer refuses status and admission",!rsx_live_draw_report_batch_admit(lifetime) &&
        rsx_live_draw_report_batch_status(lifetime,g.fence_value,1)==-1);
    const int reached_queue=strcmp(mode,"close")!=0;
    const int reached_reset=!strcmp(mode,"reset")||!strcmp(mode,"list-reset");
    ldh_check("no extra submissions or resets after original failure",closes==1 &&
        executes==(unsigned)reached_queue && signals==(unsigned)reached_queue &&
        allocator_resets==(unsigned)reached_reset && list_resets==(unsigned)(!strcmp(mode,"list-reset")));
    ldh_check("unsafe resource owners retained until real completion",g.n_retired_textures==
        ((!strcmp(mode,"reset")||!strcmp(mode,"list-reset"))?0u:1u));
    if(!strcmp(mode,"wait"))ldh_check("original event wait failure stays fail-closed",checked_waits==1);
    /* Restore real interface tables; do not fake readiness or reclaim failed work. */
    g.list->lpVtbl=real_list;g.alloc->lpVtbl=real_allocator;g.queue->lpVtbl=real_queue;
    g.fence->lpVtbl=real_fence;g.dev->lpVtbl=real_device;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "mip-pressure";
    if (ldh_init("test_live_draw_report_batch")) return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(), SW_HIDE);
    ldh_check("initial white texture upload remains available", g.white_tex != NULL);
    if (!strcmp(mode,"pressure-failure")) {
        ldh_setup_quad(0,1,1,0,0);rsx_test_fp_tex_r0_tc0(ldh_main+0x1000,0);
        ldh_surface(8u|(1u<<8),0x10000,32,0x200000,64,8,8);
        memset(ldh_local+0x50000,255,256);ldh_texture0(0x50000,0xa5,8,8,32);
        g.upload_used=UPLOAD_SIZE-256;failure_mode="close";
        real_list=g.list->lpVtbl;list_proxy=*real_list;
        list_proxy.Close=fail_close;list_proxy.SetPipelineState=count_pipeline;g.list->lpVtbl=&list_proxy;
        ldh_draw(0x1000,0x40,0,0,0x207);
        ldh_check("pressure failure revokes readiness",!g.ready && closes==1);
        ldh_check("failed preparation cannot record the remainder of the draw",!recording_after_failure);
        g.list->lpVtbl=real_list;
    } else if (!strcmp(mode,"status-failures")) {
        const u64 lifetime=rsx_live_draw_report_batch_lifetime();
        const u64 ticket=rsx_live_draw_report_batch_issue(lifetime),before=g.fence_value;
        real_fence=g.fence->lpVtbl;real_device=g.dev->lpVtbl;
        fence_proxy=*real_fence;fence_proxy.GetCompletedValue=fail_completed;
        device_proxy=*real_device;device_proxy.GetDeviceRemovedReason=fail_device;
        g.fence->lpVtbl=&fence_proxy;g.dev->lpVtbl=&device_proxy;
        failure_mode="fence";
        ldh_check("status rejects UINT64_MAX device-removed sentinel",rsx_live_draw_report_batch_status(lifetime,ticket,0)==-1);
        failure_mode="incomplete";
        ldh_check("status never manufactures incomplete completion",rsx_live_draw_report_batch_status(lifetime,ticket,1)==-1);
        failure_mode="device";
        ldh_check("status rejects device removal after completion",rsx_live_draw_report_batch_status(lifetime,ticket,1)==-1);
        ldh_check("all status failures leave submission count unchanged",g.fence_value==before);
        g.fence->lpVtbl=real_fence;g.dev->lpVtbl=real_device;
    } else if (!strncmp(mode,"failure-",8)) failure_case(mode+8);
    else if (!strcmp(mode,"draw-pressure") || !strcmp(mode,"multi-texture-pressure")) {
        const int multi=!strcmp(mode,"multi-texture-pressure");
        ldh_setup_quad(0,1,1,0,0);
        rsx_test_fp_tex_r0_tc0(ldh_main+0x1000,0);
        if(multi) {
            rsx_test_fp_instr(ldh_main+0x1000,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX)|(1u<<9)|
                RSX_TEST_FP_INSRC_TC0,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
            rsx_test_fp_instr(ldh_main+0x1010,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX)|(1u<<10)|
                RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_TEXU(1)|RSX_TEST_FP_END,
                RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
        }
        ldh_surface(8u|(1u<<8),0x10000,32,0x200000,64,8,8);
        for(u32 i=0;i<64;i++) {
            u8* red=ldh_local+0x50000+i*4;red[0]=255;red[1]=255;red[2]=red[3]=0;
            u8* green=ldh_local+0x60000+i*4;green[0]=255;green[1]=0;green[2]=255;green[3]=0;
        }
        ldh_texture0(0x50000,0xa5,8,8,32);
        if(!multi)ldh_draw(0x1000,0x40,0,0,0x207);
        const u64 before=g.fence_value;g.upload_used=UPLOAD_SIZE-(multi?2560u:256u);
        if(multi) {
            const u32 unit=32;
            ldh_m(NV4097_SET_TEXTURE_OFFSET+unit,0x60000);
            ldh_m(NV4097_SET_TEXTURE_FORMAT+unit,1u|(2u<<4)|(0xa5u<<8)|(1u<<16));
            ldh_m(NV4097_SET_TEXTURE_ADDRESS+unit,3u|(3u<<8)|(3u<<16));
            ldh_m(NV4097_SET_TEXTURE_CONTROL0+unit,0x80000000u|((12u<<8)<<7));
            ldh_m(NV4097_SET_TEXTURE_CONTROL1+unit,0xaae4);
            ldh_m(NV4097_SET_TEXTURE_CONTROL3+unit,32);
            ldh_m(NV4097_SET_TEXTURE_FILTER+unit,(1u<<16)|(1u<<24)|(1u<<13));
            ldh_m(NV4097_SET_TEXTURE_IMAGE_RECT+unit,(8u<<16)|8u);
        } else {
            ldh_m(NV4097_SET_SCISSOR_HORIZONTAL,(4u<<16)|4u);
            ldh_texture0(0x60000,0xa5,8,8,32);
        }
        ldh_draw(0x1000,0x40,0,0,0x207);
        ldh_check("texture pressure performs exactly one genuine preparation flush",g.ready && g.fence_value==before+1);
        u8 rgb[8*8*3];u32 w=0,h=0;
        const int read=!ldh_read_rgb(0x10000,"draw-pressure.ppm",rgb,8,8,&w,&h);
        const u8* left=rgb+(4*8+1)*3;const u8* right=rgb+(4*8+6)*3;
        ldh_check("preparation flush preserves all texture bindings and final render state",read && w==8 && h==8 &&
            left[0]>250 && left[1]<(multi?256:3) && left[2]<3 &&
            right[0]<(multi?256:3) && right[1]>250 && right[2]<3 &&
            (!multi || (left[1]>250 && right[0]>250)));
    } else if (!strcmp(mode,"mip-pressure") || !strcmp(mode,"cube-pressure"))
        upload_pressure(!strcmp(mode,"cube-pressure"));
    else if (!strcmp(mode,"oversize")) {
        const u8 unused=0;
        const tex_level_t huge = {1,1,&unused,UPLOAD_SIZE + 256u,1};
        const u64 before = g.fence_value; const u32 upload_before=g.upload_used;
        ID3D12Resource* texture = create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM,&huge,1);
        ldh_check("oversized whole upload rejected before any command or flush",
            !texture && g.ready && g.fence_value == before && g.upload_used == upload_before);
        if (texture) retire_texture(texture);
    } else if (!strcmp(mode,"adapter")) {
#ifndef RSX_LIVE_HAS_REPORT_BATCH
        ldh_check("LIVE completed report adapter is available", 0);
#else
        const u64 lifetime = rsx_live_draw_report_batch_lifetime();
        ldh_check("renderer has a live nonzero lifetime", lifetime != 0);
        ldh_check("admission checks original position", rsx_live_draw_report_batch_admit(lifetime));
        const u64 before = g.fence_value;
        const u64 ticket = rsx_live_draw_report_batch_issue(lifetime);
        ldh_check("issue returns the actual newly completed fence", ticket == before + 1 &&
            ticket == g.fence_value && g.fence->lpVtbl->GetCompletedValue(g.fence) >= ticket);
        const u64 submitted = g.fence_value;
        ldh_check("status validates a completed fence without another submission",
            rsx_live_draw_report_batch_status(lifetime,ticket,0) == 1 &&
            rsx_live_draw_report_batch_status(lifetime,ticket,1) == 1 && g.fence_value == submitted);
        ldh_check("zero and future tickets rejected", rsx_live_draw_report_batch_status(lifetime,0,0) == -1 &&
            rsx_live_draw_report_batch_status(lifetime,ticket + 1,1) == -1);
        ldh_check("wrong renderer lifetime rejected", !rsx_live_draw_report_batch_admit(lifetime+1) &&
            !rsx_live_draw_report_batch_issue(lifetime+1) &&
            rsx_live_draw_report_batch_status(lifetime+1,ticket,0) == -1);
        ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
        ldh_check("partial primitive rejected at original position", !rsx_live_draw_report_batch_admit(lifetime) &&
            !rsx_live_draw_report_batch_issue(lifetime) && g.fence_value == submitted);
        ldh_m(NV4097_SET_BEGIN_END,0);
        ldh_check("normal admission resumes after primitive ends", rsx_live_draw_report_batch_admit(lifetime));
        rsx_live_draw_shutdown();
        ldh_check("shutdown invalidates old tickets", !rsx_live_draw_report_batch_lifetime() &&
            rsx_live_draw_report_batch_status(lifetime,ticket,0) == -1);
        rsx_null_backend_shutdown();
        ldh_check("new host window created", rsx_null_backend_init(64,64,"report-reinit") == 0);
        ldh_check("physical reinit succeeds", rsx_live_draw_init(rsx_null_backend_get_hwnd(),64,64,ldh_guest_ptr,NULL) == 0);
        ldh_check("physical reinit never reuses lifetime", rsx_live_draw_report_batch_lifetime() != lifetime &&
            !rsx_live_draw_report_batch_admit(lifetime));
#endif
    } else if (!strcmp(mode,"lease")) {
        ldh_check("paired lease installed", rsx_live_draw_set_report_lease(lease_begin,lease_end));
        ldh_check("mismatched lease rejects without mutation", !rsx_live_draw_set_report_lease(lease_begin,NULL));
        rsx_live_draw_set_report_barrier(report_barrier);
        const u32 slot = surface_get(0,0x50000,8,8,DXGI_FORMAT_R8G8B8A8_UNORM);
        ldh_check("real surface created",slot == 0);
        g.display_buffers[0].valid=1;g.display_buffers[0].offset=0x9000;
        rsx_live_draw_note_resolve(0,0x50000,0,0x9000);
        ldh_check("whole effect has one balanced lease",lease_begins == 1 && lease_ends == 1 && !lease_depth && barrier_calls == 1);
        deny_barrier=1;g.display_buffers[0].resolve_src=0;
        rsx_live_draw_note_resolve(0,0x50000,0,0x9000);
        rsx_live_draw_shutdown();
        ldh_check("failed barrier suppresses effects and teardown",g.ready && !g.display_buffers[0].resolve_src &&
            lease_begins == 3 && lease_ends == 3 && !lease_depth && barrier_calls == 3);
        ldh_check("failed barrier suppresses reinit",rsx_live_draw_init(NULL,0,0,NULL,NULL)==-1 && g.ready);
        deny_barrier=0;g.ready=0;
        rsx_live_draw_blit(0,0,0,0,0,0,0,0);
        rsx_live_draw_resolve_blit(0,0,0,0,0,0,0,0,0,0,0,0);
        rsx_live_draw_present(0);rsx_live_draw_present_rgba(NULL,0,0);
        rsx_live_draw_set_movie_mode(0);rsx_live_draw_debug_dump_surface(0,0,NULL);
        ldh_check("all host effect early returns balance their lease",lease_begins==10 &&
            lease_ends==10 && barrier_calls==10 && !lease_depth);
        g.ready=1;
        rsx_live_draw_set_report_barrier(NULL);
        ldh_check("paired NULL lease disables registration",rsx_live_draw_set_report_lease(NULL,NULL));
    } else return 3;
    rsx_live_draw_shutdown();
    return ldh_done("test_live_draw_report_batch");
}
