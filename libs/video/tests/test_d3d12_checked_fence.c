/* Actual fence completion helper; synthetic COM endpoints, no GPU or game. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include "../rsx_d3d12_checked_fence.h"
static UINT64 before,after;
static unsigned queries,events,waits;
static HRESULT removed,event_result;
static DWORD wait_result;
static UINT64 STDMETHODCALLTYPE completed(ID3D12Fence* self) {
    (void)self;return queries++ ? after : before;
}
static HRESULT STDMETHODCALLTYPE event_on(ID3D12Fence* self,UINT64 value,HANDLE event) {
    (void)self;(void)value;(void)event;events++;return event_result;
}
static HRESULT STDMETHODCALLTYPE removal(ID3D12Device* self) {(void)self;return removed;}
static DWORD WINAPI waited(HANDLE event,DWORD timeout) {(void)event;(void)timeout;waits++;return wait_result;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
    ID3D12FenceVtbl fence_methods={0};fence_methods.GetCompletedValue=completed;fence_methods.SetEventOnCompletion=event_on;
    ID3D12DeviceVtbl device_methods={0};device_methods.GetDeviceRemovedReason=removal;
    ID3D12Fence fence={&fence_methods};ID3D12Device device={&device_methods};
    HANDLE event=(HANDLE)(uintptr_t)1;
    before=12;after=12;wait_result=WAIT_OBJECT_0;
    CHECK(rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited)==1 && !events && !waits);
    queries=events=waits=0;before=0;after=12;
    CHECK(rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited)==1 && events==1 && waits==1);
    queries=events=waits=0;before=UINT64_MAX;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited) && !events && !waits);
    queries=0;before=12;removed=DXGI_ERROR_DEVICE_REMOVED;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited));
    removed=S_OK;before=0;queries=0;event_result=E_FAIL;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited));
    event_result=S_OK;queries=0;wait_result=WAIT_TIMEOUT;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited));
    queries=0;wait_result=WAIT_FAILED;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited));
    queries=0;wait_result=WAIT_OBJECT_0;after=11;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited));
    queries=0;after=UINT64_MAX;
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,waited));
    CHECK(!rsx_d3d12_wait_checked(NULL,&fence,event,12,100,waited));
    CHECK(!rsx_d3d12_wait_checked(&device,NULL,event,12,100,waited));
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,NULL,12,100,waited));
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,UINT64_MAX,100,waited));
    CHECK(!rsx_d3d12_wait_checked(&device,&fence,event,12,100,NULL));
    puts("PASS checked D3D12 fence: completion, device loss, event failure, timeout, failed and early waits");
    return 0;
}
