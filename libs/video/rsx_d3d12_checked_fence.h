#ifndef PS3RECOMP_D3D12_CHECKED_FENCE_H
#define PS3RECOMP_D3D12_CHECKED_FENCE_H
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

/* A signaled event is only a wakeup. Check the actual queue fence again before
 * reclaiming uploads or publishing a guest completion. UINT64_MAX denotes a
 * removed device, never successful completion (ID3D12Fence::GetCompletedValue).
 * The wait function is explicit so failure cases remain deterministic in tests. */
typedef DWORD (WINAPI *rsx_d3d12_wait_fn)(HANDLE, DWORD);
static inline int rsx_d3d12_wait_checked(ID3D12Device* device, ID3D12Fence* fence,
                                       HANDLE event, uint64_t value,
                                       DWORD timeout, rsx_d3d12_wait_fn wait_fn)
{
    if (!device || !fence || !event || !wait_fn || value == UINT64_MAX) return 0;
    uint64_t completed = fence->lpVtbl->GetCompletedValue(fence);
    if (completed == UINT64_MAX) return 0;
    if (completed < value) {
        if (FAILED(fence->lpVtbl->SetEventOnCompletion(fence, value, event)) ||
            wait_fn(event, timeout) != WAIT_OBJECT_0) return 0;
        completed = fence->lpVtbl->GetCompletedValue(fence);
    }
    return completed != UINT64_MAX && completed >= value &&
           device->lpVtbl->GetDeviceRemovedReason(device) == S_OK;
}
#endif
